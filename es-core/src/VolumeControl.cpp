// SPDX-License-Identifier: MIT
// Copyright (C) 2026-present PortareOS (https://github.com/portare-ch)

#include "VolumeControl.h"

#include "math/Misc.h"
#include "Log.h"
#include "Settings.h"

#include <cmath>
#include <cstring>
#include <cerrno>
#include <map>
#include <string>
#include <pipewire/pipewire.h>
#include <pipewire/extensions/metadata.h>
#include <spa/param/props.h>
#include <spa/param/audio/raw.h>
#include <spa/pod/builder.h>
#include <spa/pod/iter.h>
#include <spa/pod/parser.h>
#include <spa/utils/json.h>
#include "utils/StringUtil.h"
#include "SystemConf.h"

// Volume against pipewire directly rather than through its pulse compatibility
// layer.
//
// Three objects have to be found before anything can be set. The metadata
// object called "default" says which sink is current, as a node *name*; the
// registry says which node *id* carries that name; and the node itself holds
// channelVolumes under SPA_PARAM_Props. The default can change under us, so the
// registry listener stays subscribed and rebinds when it does.
//
// channelVolumes are linear amplitude. The number in the UI is not: pulse, and
// wireplumber after it, present a cubic curve, so the same 30% has to mean the
// same loudness it did before or every user's setting quietly changes meaning.
class PipeWireControl
{
public:
	PipeWireControl()
	{
		mLoop = nullptr;
		mContext = nullptr;
		mCore = nullptr;
		mRegistry = nullptr;
		mMetadata = nullptr;
		mNode = nullptr;
		mNodeId = SPA_ID_INVALID;
		mReady = false;
		mVolume = 100;

		pw_init(nullptr, nullptr);

		mLoop = pw_thread_loop_new("es-volume", nullptr);
		if (mLoop == nullptr)
		{
			LOG(LogError) << "PipeWireControl: pw_thread_loop_new failed";
			return;
		}

		// Start before connecting, not after. pw_context_connect needs the
		// loop iterating to complete its handshake, and with the loop stopped
		// it returns null, which is how volume ended up permanently
		// unavailable while playback itself was fine.
		if (pw_thread_loop_start(mLoop) < 0)
		{
			LOG(LogError) << "PipeWireControl: pw_thread_loop_start failed";
			return;
		}

		pw_thread_loop_lock(mLoop);

		mContext = pw_context_new(pw_thread_loop_get_loop(mLoop), nullptr, 0);
		if (mContext == nullptr)
			LOG(LogError) << "PipeWireControl: pw_context_new failed";
		else
		{
			mCore = pw_context_connect(mContext, nullptr, 0);
			if (mCore == nullptr)
				LOG(LogError) << "PipeWireControl: pw_context_connect failed: " << strerror(errno);
		}

		if (mCore != nullptr)
		{
			mRegistry = pw_core_get_registry(mCore, PW_VERSION_REGISTRY, 0);
			if (mRegistry == nullptr)
				LOG(LogError) << "PipeWireControl: pw_core_get_registry failed";
			else
			{
				spa_zero(mRegistryListener);
				pw_registry_add_listener(mRegistry, &mRegistryListener, &sRegistryEvents, this);
				mReady = true;
			}
		}

		pw_thread_loop_unlock(mLoop);

		LOG(LogInfo) << "PipeWireControl. Ready = " << mReady;
	}

	~PipeWireControl()
	{
		exit();
	}

	bool isReady() { return mReady; }

	int getVolume()
	{
		return mVolume;
	}

	void setVolume(int value, bool setSinkVolume = true)
	{
		mVolume = Math::clamp(value, 0, 100);

		if (!mReady || !setSinkVolume)
			return;

		pw_thread_loop_lock(mLoop);
		applyVolume();
		pw_thread_loop_unlock(mLoop);
	}

	void exit()
	{
		if (mLoop == nullptr)
			return;

		LOG(LogDebug) << "PipeWireControl.exit";

		mReady = false;

		pw_thread_loop_stop(mLoop);

		if (mNode != nullptr) { pw_proxy_destroy((pw_proxy*)mNode); mNode = nullptr; }
		if (mMetadata != nullptr) { pw_proxy_destroy((pw_proxy*)mMetadata); mMetadata = nullptr; }
		if (mRegistry != nullptr) { pw_proxy_destroy((pw_proxy*)mRegistry); mRegistry = nullptr; }
		if (mCore != nullptr) { pw_core_disconnect(mCore); mCore = nullptr; }
		if (mContext != nullptr) { pw_context_destroy(mContext); mContext = nullptr; }

		pw_thread_loop_destroy(mLoop);
		mLoop = nullptr;
	}

private:
	// The loop is already locked by every caller below.
	void applyVolume()
	{
		if (mNode == nullptr)
			return;

		float linear = (float)std::pow(mVolume / 100.0, 3.0);

		float volumes[SPA_AUDIO_MAX_CHANNELS];
		uint32_t channels = mChannels > 0 ? mChannels : 2;
		if (channels > SPA_AUDIO_MAX_CHANNELS)
			channels = SPA_AUDIO_MAX_CHANNELS;

		for (uint32_t i = 0; i < channels; i++)
			volumes[i] = linear;

		uint8_t buffer[1024];
		spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));

		spa_pod_frame f;
		spa_pod_builder_push_object(&b, &f, SPA_TYPE_OBJECT_Props, SPA_PARAM_Props);
		spa_pod_builder_prop(&b, SPA_PROP_channelVolumes, 0);
		spa_pod_builder_array(&b, sizeof(float), SPA_TYPE_Float, channels, volumes);
		spa_pod* pod = (spa_pod*)spa_pod_builder_pop(&b, &f);

		pw_node_set_param(mNode, SPA_PARAM_Props, 0, pod);
		pw_core_sync(mCore, PW_ID_CORE, 0);
	}

	void bindDefaultSink(uint32_t id)
	{
		if (mNode != nullptr)
		{
			pw_proxy_destroy((pw_proxy*)mNode);
			mNode = nullptr;
		}

		mNodeId = id;
		mNode = (pw_node*)pw_registry_bind(mRegistry, id, PW_TYPE_INTERFACE_Node, PW_VERSION_NODE, 0);
		if (mNode == nullptr)
			return;

		spa_zero(mNodeListener);
		pw_node_add_listener(mNode, &mNodeListener, &sNodeEvents, this);

		uint32_t ids[] = { SPA_PARAM_Props };
		pw_node_subscribe_params(mNode, ids, 1);
	}

	// A sink is only interesting once its name matches what the metadata says
	// is default. Either can arrive first, so both paths end up here.
	void considerSink(uint32_t id, const char* name)
	{
		if (name == nullptr)
			return;

		mSinks[id] = name;

		if (!mDefaultSink.empty() && mDefaultSink == name && mNodeId != id)
			bindDefaultSink(id);
	}

	void onDefaultSinkChanged(const std::string& name)
	{
		mDefaultSink = name;

		for (auto& sink : mSinks)
		{
			if (sink.second == name && mNodeId != sink.first)
			{
				bindDefaultSink(sink.first);
				return;
			}
		}
	}

	static void registry_global(void* data, uint32_t id, uint32_t /*permissions*/,
		const char* type, uint32_t /*version*/, const spa_dict* props)
	{
		PipeWireControl* pThis = (PipeWireControl*)data;

		if (props == nullptr || type == nullptr)
			return;

		if (strcmp(type, PW_TYPE_INTERFACE_Node) == 0)
		{
			const char* mediaClass = spa_dict_lookup(props, PW_KEY_MEDIA_CLASS);
			if (mediaClass != nullptr && strcmp(mediaClass, "Audio/Sink") == 0)
				pThis->considerSink(id, spa_dict_lookup(props, PW_KEY_NODE_NAME));
		}
		else if (strcmp(type, PW_TYPE_INTERFACE_Metadata) == 0 && pThis->mMetadata == nullptr)
		{
			const char* name = spa_dict_lookup(props, PW_KEY_METADATA_NAME);
			if (name == nullptr || strcmp(name, "default") != 0)
				return;

			pThis->mMetadata = (pw_metadata*)pw_registry_bind(pThis->mRegistry, id,
				PW_TYPE_INTERFACE_Metadata, PW_VERSION_METADATA, 0);

			if (pThis->mMetadata != nullptr)
			{
				spa_zero(pThis->mMetadataListener);
				pw_metadata_add_listener(pThis->mMetadata, &pThis->mMetadataListener, &sMetadataEvents, pThis);
			}
		}
	}

	static void registry_global_remove(void* data, uint32_t id)
	{
		PipeWireControl* pThis = (PipeWireControl*)data;

		pThis->mSinks.erase(id);

		if (pThis->mNodeId == id && pThis->mNode != nullptr)
		{
			pw_proxy_destroy((pw_proxy*)pThis->mNode);
			pThis->mNode = nullptr;
			pThis->mNodeId = SPA_ID_INVALID;
		}
	}

	// default.audio.sink arrives as {"name":"<node.name>"}.
	static int metadata_property(void* data, uint32_t /*subject*/, const char* key,
		const char* /*type*/, const char* value)
	{
		PipeWireControl* pThis = (PipeWireControl*)data;

		if (key == nullptr || value == nullptr || strcmp(key, "default.audio.sink") != 0)
			return 0;

		spa_json it[2];
		char name[256] = { 0 };
		char k[128];

		spa_json_init(&it[0], value, strlen(value));
		if (spa_json_enter_object(&it[0], &it[1]) <= 0)
			return 0;

		while (spa_json_get_string(&it[1], k, sizeof(k)) > 0)
		{
			if (strcmp(k, "name") == 0)
			{
				if (spa_json_get_string(&it[1], name, sizeof(name)) > 0)
					pThis->onDefaultSinkChanged(name);

				break;
			}

			const char* skip;
			if (spa_json_next(&it[1], &skip) <= 0)
				break;
		}

		return 0;
	}

	static void node_param(void* data, int /*seq*/, uint32_t id, uint32_t /*index*/,
		uint32_t /*next*/, const spa_pod* param)
	{
		PipeWireControl* pThis = (PipeWireControl*)data;

		if (param == nullptr || id != SPA_PARAM_Props)
			return;

		const spa_pod_prop* prop = spa_pod_find_prop(param, nullptr, SPA_PROP_channelVolumes);
		if (prop == nullptr)
			return;

		float volumes[SPA_AUDIO_MAX_CHANNELS];
		uint32_t n = spa_pod_copy_array(&prop->value, SPA_TYPE_Float, volumes, SPA_AUDIO_MAX_CHANNELS);

		if (n == 0)
			return;

		pThis->mChannels = n;
		pThis->mVolume = (int)std::lround(std::cbrt((double)volumes[0]) * 100.0);
	}

private:
	pw_thread_loop*	mLoop;
	pw_context*		mContext;
	pw_core*		mCore;
	pw_registry*	mRegistry;
	pw_metadata*	mMetadata;
	pw_node*		mNode;

	spa_hook		mRegistryListener;
	spa_hook		mMetadataListener;
	spa_hook		mNodeListener;

	uint32_t		mNodeId;
	uint32_t		mChannels = 0;
	std::string		mDefaultSink;
	std::map<uint32_t, std::string> mSinks;

	bool			mReady;
	int				mVolume;

	static const pw_registry_events	sRegistryEvents;
	static const pw_metadata_events	sMetadataEvents;
	static const pw_node_events		sNodeEvents;
};

const pw_registry_events PipeWireControl::sRegistryEvents = {
	PW_VERSION_REGISTRY_EVENTS,
	PipeWireControl::registry_global,
	PipeWireControl::registry_global_remove,
};

const pw_metadata_events PipeWireControl::sMetadataEvents = {
	PW_VERSION_METADATA_EVENTS,
	PipeWireControl::metadata_property,
};

const pw_node_events PipeWireControl::sNodeEvents = {
	PW_VERSION_NODE_EVENTS,
	nullptr,
	PipeWireControl::node_param,
};

static PipeWireControl PipeWire;

std::weak_ptr<VolumeControl> VolumeControl::sInstance;

VolumeControl::VolumeControl() : internalVolume(0)
{
	init();
}

VolumeControl::~VolumeControl()
{
	if (PipeWire.isReady())
		PipeWire.exit();
}

std::shared_ptr<VolumeControl> & VolumeControl::getInstance()
{
	static std::shared_ptr<VolumeControl> sharedInstance = sInstance.lock();
	if (sharedInstance == nullptr)
	{
		sharedInstance.reset(new VolumeControl);
		sInstance = sharedInstance;
	}

	return sharedInstance;
}

void VolumeControl::init()
{
	// The volume the user last chose, which pipewire has no memory of. Applied
	// locally rather than pushed at the sink, so starting up does not overwrite
	// a volume something else has since set.
	std::string volume = SystemConf::getInstance()->get("audio.volume");
	PipeWire.setVolume(volume.empty() ? 100 : Utils::String::toInteger(volume), false);
}

void VolumeControl::deinit()
{
}

int VolumeControl::getVolume() const
{
	return Math::clamp(PipeWire.getVolume(), 0, 100);
}

void VolumeControl::setVolume(int volume)
{
	internalVolume = Math::clamp(volume, 0, 100);

	// Unconditional. PipeWireControl records the value either way and only
	// skips the sink when it has nothing to talk to, so the on-screen bar and
	// audio.volume still follow the buttons even if the graph is unreachable.
	PipeWire.setVolume(internalVolume);
}

bool VolumeControl::isAvailable()
{
	return PipeWire.isReady();
}
