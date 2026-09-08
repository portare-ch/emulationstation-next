#include "VolumeControl.h"

#include "math/Misc.h"
#include "Log.h"
#include "Settings.h"

#ifdef WIN32
#include <mmdeviceapi.h>
#endif

#ifdef _ENABLE_PIPEWIRE_
#include <cmath>
#include <cstring>
#include <map>
#include <string>
#include <pipewire/pipewire.h>
#include <spa/param/props.h>
#include <spa/pod/builder.h>
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
			return;

		pw_thread_loop_lock(mLoop);

		mContext = pw_context_new(pw_thread_loop_get_loop(mLoop), nullptr, 0);
		if (mContext != nullptr)
			mCore = pw_context_connect(mContext, nullptr, 0);

		if (mCore != nullptr)
		{
			mRegistry = pw_core_get_registry(mCore, PW_VERSION_REGISTRY, 0);
			if (mRegistry != nullptr)
			{
				spa_zero(mRegistryListener);
				pw_registry_add_listener(mRegistry, &mRegistryListener, &sRegistryEvents, this);
				mReady = true;
			}
		}

		pw_thread_loop_unlock(mLoop);

		if (mReady && pw_thread_loop_start(mLoop) < 0)
			mReady = false;

		LOG(LogDebug) << "PipeWireControl. Ready = " << mReady;
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

		if (spa_json_begin_object(&it[0], value, strlen(value)) <= 0)
			return 0;

		char k[128];
		while (spa_json_get_string(&it[0], k, sizeof(k)) > 0)
		{
			if (strcmp(k, "name") == 0)
			{
				if (spa_json_get_string(&it[0], name, sizeof(name)) > 0)
					pThis->onDefaultSinkChanged(name);

				break;
			}

			if (spa_json_next(&it[0], &value) <= 0)
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

		const spa_pod* value = spa_pod_find_prop(param, nullptr, SPA_PROP_channelVolumes);
		if (value == nullptr)
			return;

		float volumes[SPA_AUDIO_MAX_CHANNELS];
		uint32_t n = spa_pod_copy_array(&((const spa_pod_prop*)value)->value,
			SPA_TYPE_Float, volumes, SPA_AUDIO_MAX_CHANNELS);

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

#endif

#if defined(__linux__)
#if defined(_RPI_) || defined(_VERO4K_)
		std::string VolumeControl::mixerName = "PCM";
#else
		std::string VolumeControl::mixerName = "Master";
#endif
	std::string VolumeControl::mixerCard = "default";
#endif

std::weak_ptr<VolumeControl> VolumeControl::sInstance;


VolumeControl::VolumeControl()
	: internalVolume(0)
#if defined (__APPLE__)
	#error TODO: Not implemented for MacOS yet!!!
#elif defined(__linux__)
	, mixerIndex(0), mixerHandle(nullptr), mixerElem(nullptr), mixerSelemId(nullptr)
#elif defined(WIN32) || defined(_WIN32)
	, mixerHandle(nullptr), endpointVolume(nullptr)
#endif
{
	init();
}

VolumeControl::~VolumeControl()
{
#ifdef _ENABLE_PIPEWIRE_
	if (PipeWire.isReady())
		PipeWire.exit();
#endif

	deinit();
}

std::shared_ptr<VolumeControl> & VolumeControl::getInstance()
{
	//check if an VolumeControl instance is already created, if not create one
	static std::shared_ptr<VolumeControl> sharedInstance = sInstance.lock();
	if (sharedInstance == nullptr) {
		sharedInstance.reset(new VolumeControl);
		sInstance = sharedInstance;
	}
	return sharedInstance;
}

void VolumeControl::init()
{
	//initialize audio mixer interface
#if defined (__APPLE__)
	#error TODO: Not implemented for MacOS yet!!!
#elif defined(__linux__)

#ifdef _ENABLE_PIPEWIRE_
	// Read initial volume from systemconf
	std::string volume = SystemConf::getInstance()->get("audio.volume");
	PipeWire.setVolume(volume.empty() ? 100 : Utils::String::toInteger(volume), false);
	return;
#endif

	//try to open mixer device
	if (mixerHandle == nullptr)
	{
		// Allow users to override the AudioCard and MixerName in es_settings.cfg
		auto audioCard = Settings::getInstance()->getString("AudioCard");
		if (!audioCard.empty())
			mixerCard = audioCard;

		auto audioDevice = Settings::getInstance()->getString("AudioDevice");
		if (!audioDevice.empty())
			mixerName = audioDevice;

		snd_mixer_selem_id_alloca(&mixerSelemId);
		//sets simple-mixer index and name
		snd_mixer_selem_id_set_index(mixerSelemId, mixerIndex);
		snd_mixer_selem_id_set_name(mixerSelemId, mixerName.c_str());
		//open mixer
		if (snd_mixer_open(&mixerHandle, 0) >= 0)
		{
			LOG(LogDebug) << "VolumeControl::init() - Opened ALSA mixer";
			//ok. attach to defualt card
			if (snd_mixer_attach(mixerHandle, mixerCard.c_str()) >= 0)
			{
				LOG(LogDebug) << "VolumeControl::init() - Attached to default card";
				//ok. register simple element class
				if (snd_mixer_selem_register(mixerHandle, NULL, NULL) >= 0)
				{
					LOG(LogDebug) << "VolumeControl::init() - Registered simple element class";
					//ok. load registered elements
					if (snd_mixer_load(mixerHandle) >= 0)
					{
						LOG(LogDebug) << "VolumeControl::init() - Loaded mixer elements";
						//ok. find elements now
						mixerElem = snd_mixer_find_selem(mixerHandle, mixerSelemId);
						if (mixerElem != nullptr)
						{
							//wohoo. good to go...
							LOG(LogDebug) << "VolumeControl::init() - Mixer initialized";
						}
						else
						{
							LOG(LogInfo) << "VolumeControl::init() - Unable to find mixer " << mixerName << " -> Search for alternative mixer";

							snd_mixer_selem_id_t *mxid = nullptr;
							snd_mixer_selem_id_alloca(&mxid);

							for (snd_mixer_elem_t* mxe = snd_mixer_first_elem(mixerHandle); mxe != nullptr; mxe = snd_mixer_elem_next(mxe))
							{
								if (snd_mixer_selem_has_playback_volume(mxe) != 0 && snd_mixer_selem_is_active(mxe) != 0)
								{
									snd_mixer_selem_get_id(mxe, mxid);
									mixerName = snd_mixer_selem_id_get_name(mxid);

									LOG(LogInfo) << "mixername : " << mixerName;

									snd_mixer_selem_id_set_name(mixerSelemId, mixerName.c_str());
									mixerElem = snd_mixer_find_selem(mixerHandle, mixerSelemId);
									if (mixerElem != nullptr)
									{
										//wohoo. good to go...
										LOG(LogDebug) << "VolumeControl::init() - Mixer initialized";
										break;
									}
									else
										LOG(LogDebug) << "VolumeControl::init() - Mixer not initialized";
								}
							}

							if (mixerElem == nullptr)
							{
								LOG(LogError) << "VolumeControl::init() - Failed to find mixer elements!";
								snd_mixer_close(mixerHandle);
								mixerHandle = nullptr;
							}
						}
					}
					else
					{
						LOG(LogError) << "VolumeControl::init() - Failed to load mixer elements!";
						snd_mixer_close(mixerHandle);
						mixerHandle = nullptr;
					}
				}
				else
				{
					LOG(LogError) << "VolumeControl::init() - Failed to register simple element class!";
					snd_mixer_close(mixerHandle);
					mixerHandle = nullptr;
				}
			}
			else
			{
				LOG(LogError) << "VolumeControl::init() - Failed to attach to default card!";
				snd_mixer_close(mixerHandle);
				mixerHandle = nullptr;
			}
		}
		else
		{
			LOG(LogError) << "VolumeControl::init() - Failed to open ALSA mixer!";
		}
	}
#elif defined(WIN32) || defined(_WIN32)
	//get windows version information
	OSVERSIONINFOEXA osVer = {sizeof(OSVERSIONINFO)};
	::GetVersionExA(reinterpret_cast<LPOSVERSIONINFOA>(&osVer));
	//check windows version
	if(osVer.dwMajorVersion < 6)
	{
		//Windows older than Vista. use mixer API. open default mixer
		if (mixerHandle == nullptr)
		{
			if (mixerOpen(&mixerHandle, 0, NULL, 0, 0) == MMSYSERR_NOERROR)
			{
				//retrieve info on the volume slider control for the "Speaker Out" line
				MIXERLINECONTROLS mixerLineControls;
				mixerLineControls.cbStruct = sizeof(MIXERLINECONTROLS);
				mixerLineControls.dwLineID = 0xFFFF0000; //Id of "Speaker Out" line
				mixerLineControls.cControls = 1;
				//mixerLineControls.dwControlID = 0x00000000; //Id of "Speaker Out" line's volume slider
				mixerLineControls.dwControlType = MIXERCONTROL_CONTROLTYPE_VOLUME; //Get volume control
				mixerLineControls.pamxctrl = &mixerControl;
				mixerLineControls.cbmxctrl = sizeof(MIXERCONTROL);
				if (mixerGetLineControls((HMIXEROBJ)mixerHandle, &mixerLineControls, MIXER_GETLINECONTROLSF_ONEBYTYPE) != MMSYSERR_NOERROR)
				{
					LOG(LogError) << "VolumeControl::getVolume() - Failed to get mixer volume control!";
					mixerClose(mixerHandle);
					mixerHandle = nullptr;
				}
			}
			else
			{
				LOG(LogError) << "VolumeControl::init() - Failed to open mixer!";
			}
		}
	}
	else 
	{
		//Windows Vista or above. use EndpointVolume API. get device enumerator
		if (endpointVolume == nullptr)
		{
			CoInitialize(nullptr);
			IMMDeviceEnumerator * deviceEnumerator = nullptr;
			CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER, __uuidof(IMMDeviceEnumerator), (LPVOID *)&deviceEnumerator);
			if (deviceEnumerator != nullptr)
			{
				//get default endpoint
				IMMDevice * defaultDevice = nullptr;
				deviceEnumerator->GetDefaultAudioEndpoint(eRender, eConsole, &defaultDevice);
				if (defaultDevice != nullptr)
				{
					//retrieve endpoint volume
					defaultDevice->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_INPROC_SERVER, nullptr, (LPVOID *)&endpointVolume);
					if (endpointVolume == nullptr)
					{
						LOG(LogError) << "VolumeControl::init() - Failed to get default audio endpoint volume!";
					}
					//release default device. we don't need it anymore
					defaultDevice->Release();
				}
				else
				{
					LOG(LogError) << "VolumeControl::init() - Failed to get default audio endpoint!";
				}
				//release device enumerator. we don't need it anymore
				deviceEnumerator->Release();
			}
			else
			{
				LOG(LogError) << "VolumeControl::init() - Failed to get audio endpoint enumerator!";
				CoUninitialize();
			}
		}
	}
#endif
}

void VolumeControl::deinit()
{
	//deinitialize audio mixer interface
#if defined (__APPLE__)
	#error TODO: Not implemented for MacOS yet!!!
#elif defined(__linux__)

#ifdef _ENABLE_PIPEWIRE_
	return;
#endif

	if (mixerHandle != nullptr) {
		snd_mixer_detach(mixerHandle, mixerCard.c_str());
		snd_mixer_free(mixerHandle);
		snd_mixer_close(mixerHandle);
		mixerHandle = nullptr;
		mixerElem = nullptr;
	}
#elif defined(WIN32) || defined(_WIN32)
	if (mixerHandle != nullptr) {
		mixerClose(mixerHandle);
		mixerHandle = nullptr;
	}
	else if (endpointVolume != nullptr) {
		endpointVolume->Release();
		endpointVolume = nullptr;
		CoUninitialize();
	}
#endif
}

int VolumeControl::getVolume() const
{
	int volume = 0;

#if defined (__APPLE__)
	#error TODO: Not implemented for MacOS yet!!!
#elif defined(__linux__)

#ifdef _ENABLE_PIPEWIRE_
	return PipeWire.getVolume();	
#endif

	if (mixerElem != nullptr)
	{
		if (mixerHandle != nullptr)
			snd_mixer_handle_events(mixerHandle);
		/*
		int mute_state;
		if (snd_mixer_selem_has_playback_switch(mixerElem)) 
		{
			snd_mixer_selem_get_playback_switch(mixerElem, SND_MIXER_SCHN_UNKNOWN, &mute_state);
			if (!mute_state) // system Muted
				return 0;
		}
		*/
		//get volume range
		long minVolume;
		long maxVolume;
		if (snd_mixer_selem_get_playback_volume_range(mixerElem, &minVolume, &maxVolume) == 0)
		{
			//ok. now get volume
			long rawVolume;
			if (snd_mixer_selem_get_playback_volume(mixerElem, SND_MIXER_SCHN_MONO, &rawVolume) == 0)
			{
				//worked. bring into range 0-100
				rawVolume -= minVolume;
				if (rawVolume > 0)
					volume = (rawVolume * 100.0) / (maxVolume - minVolume) + 0.5;
			}
			else
			{
				LOG(LogError) << "VolumeControl::getVolume() - Failed to get mixer volume!";
			}
		}
		else
		{
			LOG(LogError) << "VolumeControl::getVolume() - Failed to get volume range!";
		}
	}
#elif defined(WIN32) || defined(_WIN32)
	if (mixerHandle != nullptr)
	{
		//Windows older than Vista. use mixer API. get volume from line control
		MIXERCONTROLDETAILS_UNSIGNED value;
		MIXERCONTROLDETAILS mixerControlDetails;
		mixerControlDetails.cbStruct = sizeof(MIXERCONTROLDETAILS);
		mixerControlDetails.dwControlID = mixerControl.dwControlID;
		mixerControlDetails.cChannels = 1; //always 1 for a MIXERCONTROL_CONTROLF_UNIFORM control
		mixerControlDetails.cMultipleItems = 0; //always 0 except for a MIXERCONTROL_CONTROLF_MULTIPLE control
		mixerControlDetails.paDetails = &value;
		mixerControlDetails.cbDetails = sizeof(MIXERCONTROLDETAILS_UNSIGNED);

		if (mixerGetControlDetails((HMIXEROBJ)mixerHandle, &mixerControlDetails, MIXER_GETCONTROLDETAILSF_VALUE) == MMSYSERR_NOERROR) 
			volume = (int)Math::round((value.dwValue * 100) / 65535.0f);
	}
	else if (endpointVolume != nullptr)
	{
		// Windows Vista or above. use EndpointVolume API
		float floatVolume = 0.0f; //0-1

		BOOL mute = FALSE;
		if (endpointVolume->GetMute(&mute) == S_OK)
		{
			if (mute)
				return 0;
		}

		if (endpointVolume->GetMasterVolumeLevelScalar(&floatVolume) == S_OK)
			volume = (int)Math::round(floatVolume * 100.0f);
	}
#endif

	// clamp to 0-100 range
	if (volume < 0)
		volume = 0;

	if (volume > 100)
		volume = 100;

	return volume;
}

void VolumeControl::setVolume(int volume)
{
	//clamp to 0-100 range
	if (volume < 0)
	{
		volume = 0;
	}
	if (volume > 100)
	{
		volume = 100;
	}
	//store values in internal variables
	internalVolume = volume;
#if defined (__APPLE__)
	#error TODO: Not implemented for MacOS yet!!!
#elif defined(__linux__)

#ifdef _ENABLE_PIPEWIRE_
	if (PipeWire.isReady())
	{
		PipeWire.setVolume(volume);
	}
	return;
#endif

	if (mixerElem != nullptr)
	{
		//get volume range
		long minVolume;
		long maxVolume;
		if (snd_mixer_selem_get_playback_volume_range(mixerElem, &minVolume, &maxVolume) == 0)
		{
			//ok. bring into minVolume-maxVolume range and set
			long rawVolume = (volume * (maxVolume - minVolume) / 100) + minVolume;
			if (snd_mixer_selem_set_playback_volume(mixerElem, SND_MIXER_SCHN_FRONT_LEFT, rawVolume) < 0 
				|| snd_mixer_selem_set_playback_volume(mixerElem, SND_MIXER_SCHN_FRONT_RIGHT, rawVolume) < 0)
			{
				LOG(LogError) << "VolumeControl::getVolume() - Failed to set mixer volume!";
			}
		}
		else
		{
			LOG(LogError) << "VolumeControl::getVolume() - Failed to get volume range!";
		}
	}
#elif defined(WIN32) || defined(_WIN32)
	if (mixerHandle != nullptr)
	{
		//Windows older than Vista. use mixer API. get volume from line control
		MIXERCONTROLDETAILS_UNSIGNED value;
		value.dwValue = (volume * 65535) / 100;
		MIXERCONTROLDETAILS mixerControlDetails;
		mixerControlDetails.cbStruct = sizeof(MIXERCONTROLDETAILS);
		mixerControlDetails.dwControlID = mixerControl.dwControlID;
		mixerControlDetails.cChannels = 1; //always 1 for a MIXERCONTROL_CONTROLF_UNIFORM control
		mixerControlDetails.cMultipleItems = 0; //always 0 except for a MIXERCONTROL_CONTROLF_MULTIPLE control
		mixerControlDetails.paDetails = &value;
		mixerControlDetails.cbDetails = sizeof(MIXERCONTROLDETAILS_UNSIGNED);
		if (mixerSetControlDetails((HMIXEROBJ)mixerHandle, &mixerControlDetails, MIXER_SETCONTROLDETAILSF_VALUE) != MMSYSERR_NOERROR)
		{
			LOG(LogError) << "VolumeControl::setVolume() - Failed to set mixer volume!";
		}
	}
	else if (endpointVolume != nullptr)
	{
		//Windows Vista or above. use EndpointVolume API
		float floatVolume = 0.0f; //0-1
		if (volume > 0) {
			floatVolume = (float)volume / 100.0f;
		}
		if (endpointVolume->SetMasterVolumeLevelScalar(floatVolume, nullptr) != S_OK)
		{
			LOG(LogError) << "VolumeControl::setVolume() - Failed to set master volume!";
		}
	}
#endif
}

bool VolumeControl::isAvailable()
{
#if defined (__APPLE__)
	return false;
#elif defined(__linux__)

#ifdef _ENABLE_PIPEWIRE_
	return PipeWire.isReady();
#endif

	return mixerHandle != nullptr && mixerElem != nullptr;
#elif defined(WIN32) || defined(_WIN32)
	return mixerHandle != nullptr || endpointVolume != nullptr;
#endif
}
