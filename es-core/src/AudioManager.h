#pragma once
#ifndef ES_CORE_AUDIO_MANAGER_H
#define ES_CORE_AUDIO_MANAGER_H

#include <SDL3/SDL_audio.h>
#include <memory>
#include <vector>
#include <SDL3_mixer/SDL_mixer.h>
#include <string> 
#include <iostream> 
#include <deque>
#include <math.h>

class Sound;
class ThemeData;

class AudioManager
{	
private:
	AudioManager();

	static std::vector<std::shared_ptr<Sound>> sSoundVector;
	static AudioManager* sInstance;
	
	// SDL3_mixer has no channels or a music slot of its own: a mixer owns
	// tracks, a track plays one MIX_Audio at a time. Music is one track
	// held open for the life of the manager, so gain and fades survive a
	// change of song.
	static MIX_Mixer* sMixer;
	MIX_Audio* mCurrentMusic;
	MIX_Track* mMusicTrack;
	void getMusicIn(const std::string &path, std::vector<std::string>& all_matching_files);
	void playMusic(const std::string& path);
	static void musicEnd_callback(void* userdata, MIX_Track* track);
	void applyMusicVolume();

	// Nothing closes the audio device otherwise. SDL keeps feeding it
	// silence, so the PipeWire sink never goes idle, which pins the sample
	// rate and keeps the DSP path up even in suspend.
	void updateIdleRelease(int deltaTime);
	bool anySoundPlaying() const;
	int mIdleTime;

	std::string mSystemName;			// Per system music folder
	std::string mCurrentSong;			// Song name displayed in pop-ups
	std::string mCurrentThemeMusicDirectory;
	std::string mCurrentMusicPath;                  //  Stores the full path of the currently playing song
	std::deque<std::string> mLastPlayed;            // Stores recently played songs

	bool		mInitialized;
	std::string	mPlayingSystemThemeSong;

public:
	static AudioManager* getInstance();
	static bool isInitialized();

	// Sound loads and plays through the same mixer. It can be null: the
	// device is released after a spell of silence and reopened on demand.
	static MIX_Mixer* getMixer() { return sMixer; }

	// Reopen the device if the idle release closed it.
	static void ensureInitialized();

	// The manager exists but may be holding no device, which is a different
	// question from isInitialized().
	static bool hasInstance() { return sInstance != nullptr; }
	
	void init();
	void deinit();

	void registerSound(std::shared_ptr<Sound> & sound);
	void unregisterSound(std::shared_ptr<Sound> & sound);

	void play();
	std::string getCurrentSongPath() const;
	void stop();

	void playRandomMusic(bool continueIfPlaying = true);
	void stopMusic(bool fadeOut=true);
	
	inline const std::string getSongName() const { return mCurrentSong; }

	bool songNameChanged() { return mSongNameChanged; }
	void resetSongNameChangedFlag() { mSongNameChanged = false; }
	
	inline bool isSongPlaying() { return (mCurrentMusic != NULL); }

	void changePlaylist(const std::shared_ptr<ThemeData>& theme, bool force = false);

	virtual ~AudioManager();

	float mMusicVolume;
	int mVideoPlaying;

	static void setVideoPlaying(bool state);
	static void update(int deltaTime);

	static int getMaxMusicVolume();

	//  New function to get the full path of the currently playing song

private:
	void playSong(const std::string& song);
	void setSongName(const std::string& song);
	void addLastPlayed(const std::string& newSong, int totalMusic);
	bool songWasPlayedRecently(const std::string& song);

	bool mSongNameChanged;
};

#endif // ES_CORE_AUDIO_MANAGER_H
