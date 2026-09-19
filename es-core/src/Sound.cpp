#include "Sound.h"

#include "AudioManager.h"
#include "Log.h"
#include "Settings.h"
#include "ThemeData.h"
#include "resources/ResourceManager.h"

std::map< std::string, std::shared_ptr<Sound> > Sound::sMap;

std::shared_ptr<Sound> Sound::get(const std::string& path)
{
	std::string file = ResourceManager::getInstance()->getResourcePath(path);

	auto it = sMap.find(file);
	if (it != sMap.cend())
		return it->second;

	std::shared_ptr<Sound> sound = std::shared_ptr<Sound>(new Sound(file));

	// Register whenever the manager exists, not only when it holds a device:
	// after an idle release it holds none, and a sound left unregistered
	// here would never be reloaded when the device comes back.
	if (AudioManager::isInitialized() || AudioManager::hasInstance())
	{
		AudioManager::getInstance()->registerSound(sound);
		sMap[file] = sound;
	}

	return sound;
}

std::shared_ptr<Sound> Sound::getFromTheme(const std::shared_ptr<ThemeData>& theme, const std::string& view, const std::string& element)
{
	LOG(LogInfo) << " req sound [" << view << "." << element << "]";

	const ThemeData::ThemeElement* elem = theme->getElement(view, element, "sound");
	if (!elem || !elem->has("path"))
	{
		LOG(LogInfo) << "   (missing)";
		return get("");
	}

	return get(elem->get<std::string>("path"));
}

Sound::Sound(const std::string & path) : mSampleData(NULL), mTrack(nullptr)
{
	loadFile(path);
}

Sound::~Sound()
{
	deinit();
}

void Sound::loadFile(const std::string & path)
{
	mPath = path;
	init();
}

void Sound::init()
{
	deinit();

	if (!AudioManager::isInitialized())
		return;

	if (mPath.empty() || !Utils::FileSystem::exists(mPath))
		return;

	if (!Settings::getInstance()->getBool("EnableSounds"))
		return;

	//load wav file via SDL
	MIX_Mixer* mixer = AudioManager::getMixer();
	if (mixer == nullptr)
		return;

	mSampleData = MIX_LoadAudio(mixer, mPath.c_str(), true);
	if (mSampleData == nullptr)
	{
		LOG(LogError) << "Error loading sound \"" << mPath << "\"!\n" << "	" << SDL_GetError();
		return;
	}

	mTrack = MIX_CreateTrack(mixer);
	if (mTrack == nullptr)
	{
		LOG(LogError) << "Error creating a track for \"" << mPath << "\"!\n" << "	" << SDL_GetError();
		MIX_DestroyAudio(mSampleData);
		mSampleData = nullptr;
	}
}

void Sound::deinit()
{
	if (mSampleData == nullptr)
		return;

	stop();

	if (mTrack != nullptr)
	{
		// The track has to let go of the audio before it can be freed.
		MIX_SetTrackAudio(mTrack, nullptr);
		MIX_DestroyTrack(mTrack);
		mTrack = nullptr;
	}

	MIX_DestroyAudio(mSampleData);
	mSampleData = nullptr;	
}

void Sound::play()
{
	if (!Settings::getInstance()->getBool("EnableSounds"))
		return;

	// The device is handed back after a spell of silence, which frees every
	// loaded sound with it. Reopen and reload on the way to playing rather
	// than going quiet for good.
	if (mSampleData == nullptr)
	{
		// reopening reloads every registered sound, so only load by hand
		// if this one was not among them
		AudioManager::ensureInitialized();
		if (mSampleData == nullptr)
			init();

		if (mSampleData == nullptr)
			return;
	}

	if (mTrack == nullptr)
		return;

	MIX_SetTrackAudio(mTrack, mSampleData);
	MIX_PlayTrack(mTrack, 0);
}

bool Sound::isPlaying() const
{	
	return mTrack != nullptr && MIX_TrackPlaying(mTrack);
}

void Sound::stop()
{
	if (mTrack == nullptr)
		return;

	MIX_StopTrack(mTrack, 0);
}
