#pragma once
#ifndef ES_CORE_SOUND_H
#define ES_CORE_SOUND_H

#include <SDL3_mixer/SDL_mixer.h>
#include <map>
#include <memory>
#include <string>

class ThemeData;

class Sound
{
	std::string mPath;
	// SDL3_mixer has no channels: a track is the thing that plays, and
	// each sound keeps its own so isPlaying() can answer honestly.
	MIX_Audio* mSampleData;
	MIX_Track* mTrack;

public:
	static std::shared_ptr<Sound> get(const std::string& path);
	static std::shared_ptr<Sound> getFromTheme(const std::shared_ptr<ThemeData>& theme, const std::string& view, const std::string& elem);

	~Sound();

	void init();
	void deinit();

	void loadFile(const std::string & path);

	void play();
	bool isPlaying() const;
	void stop();

private:
	Sound(const std::string & path = "");
	static std::map< std::string, std::shared_ptr<Sound> > sMap;
};

#endif // ES_CORE_SOUND_H
