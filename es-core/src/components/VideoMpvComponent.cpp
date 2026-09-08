// SPDX-License-Identifier: MIT
// Copyright (C) 2026-present PortareOS (https://github.com/portare-ch)

#include "components/VideoMpvComponent.h"

#include "renderers/Renderer.h"
#include "resources/TextureResource.h"
#include "utils/StringUtil.h"
#include "PowerSaver.h"
#include "Settings.h"
#include <mpv/client.h>
#include <mpv/render.h>
#include <cstring>
#include <cstdint>
#include <SDL_mutex.h>
#include <cmath>
#include "SystemConf.h"
#include "ThemeData.h"
#include <SDL_timer.h>
#include "AudioManager.h"

#ifdef WIN32
#include <codecvt>
#endif

#include "ImageIO.h"

#define MATHPI          3.141592653589793238462643383279502884L

// mpv hands a frame over when asked rather than pushing one from a thread of its
// own, so readFrame() does the work from update() and the surface mutexes are
// never contended.
VideoMpvComponent::VideoMpvComponent(Window* window) : VideoComponent(window), 
	mMpv(nullptr), mMpvRender(nullptr),
	mTopLeftCrop(0.0f, 0.0f), mBottomRightCrop(1.0f, 1.0f), mContext(nullptr)
{
	mIsParsing = false;
	mReachedEnd = false;
	mHasAudioTrack = false;
	mSaturation = 1.0f;
	mElapsed = 0;
	mColorShift = 0xFFFFFFFF;
	mLinearSmooth = false;

	mLoops = -1;
	mCurrentLoop = 0;

	// Get an empty texture for rendering the video
	mTexture = nullptr;// TextureResource::get("");
	mEffect = VideoMpvFlags::VideoMpvEffect::BUMP;
}

// mpv_terminate_destroy waits for its threads, so it is not done on the UI thread.
static void mpv_release_async(VideoContext* ctx, mpv_handle* mpv)
{
	if (ctx)
		ctx->component = nullptr;

	std::thread([mpv, ctx]()
		{
			if (mpv) mpv_terminate_destroy(mpv);
			if (ctx) delete ctx;
		}).detach();
}

VideoMpvComponent::~VideoMpvComponent()
{
	stopVideo();
}

Vector2f VideoMpvComponent::getSize() const
{
	if (mTargetIsMax && mPadding != Vector4f::Zero())
	{
		auto targetSize = mTargetSize - mPadding.xy() - mPadding.zw();

		if (mSize.x() == targetSize.x())
			return Vector2f(mSize.x() + mPadding.x() + mPadding.z(), mSize.y());
		else if (mSize.y() == targetSize.y())
			return Vector2f(mSize.x(), mSize.y() + mPadding.y() + mPadding.w());
	}

	return GuiComponent::getSize() * (mBottomRightCrop - mTopLeftCrop);
}

void VideoMpvComponent::setResize(float width, float height)
{
	if (mSize.x() != 0 && mSize.y() != 0 && !mTargetIsMax && !mTargetIsMin && mTargetSize.x() == width && mTargetSize.y() == height)
		return;

	mTargetSize = Vector2f(width, height);
	mSize = mTargetSize;
	mTargetIsMax = false;
	mTargetIsMin = false;
	mStaticImage.setMaxSize(width, height);
	resize();
}

void VideoMpvComponent::setMaxSize(float width, float height)
{
	if (mSize.x() != 0 && mSize.y() != 0 && mTargetIsMax && !mTargetIsMin && mTargetSize.x() == width && mTargetSize.y() == height)
		return;

	mTargetSize = Vector2f(width, height);
	mSize = mTargetSize;
	mTargetIsMax = true;
	mTargetIsMin = false;
	mStaticImage.setMaxSize(width, height);
	resize();
}

void VideoMpvComponent::setMinSize(float width, float height)
{
	if (mSize.x() != 0 && mSize.y() != 0 && mTargetIsMin && !mTargetIsMax && mTargetSize.x() == width && mTargetSize.y() == height)
		return;

	mTargetSize = Vector2f(width, height);
	mSize = mTargetSize;
	mTargetIsMax = false;
	mTargetIsMin = true;
	mStaticImage.setMaxSize(width, height);
	resize();
}

void VideoMpvComponent::onVideoStarted()
{
	VideoComponent::onVideoStarted();
	resize();
}

void VideoMpvComponent::crop(float left, float top, float right, float bot)
{
	mTopLeftCrop.x() = Math::clamp(left, 0.0f, 1.0f);
	mTopLeftCrop.y() = Math::clamp(top, 0.0f, 1.0f);
	mBottomRightCrop.x() = 1.0f - Math::clamp(right, 0.0f, 1.0f);
	mBottomRightCrop.y() = 1.0f - Math::clamp(bot, 0.0f, 1.0f);
}

void VideoMpvComponent::resize()
{
	if (!mTexture)
		return;

	const Vector2f textureSize((float)mVideoWidth, (float)mVideoHeight);

	if (textureSize == Vector2f::Zero())
		return;

	auto targetSize = mTargetSize - mPadding.xy() - mPadding.zw();

	if (mTargetIsMax)
	{
		crop(0, 0, 0, 0);
		mSize = textureSize;

		Vector2f resizeScale((targetSize.x() / mSize.x()), (targetSize.y() / mSize.y()));

		if (resizeScale.x() < resizeScale.y())
		{
			mSize[0] *= resizeScale.x(); // this will be mTargetSize.x(). We can't exceed it, nor be lower than it.
			// we need to make sure we're not creating an image larger than max size
			//mSize[1] = Math::min(Math::round(mSize[1] *= resizeScale.x()), mTargetSize.y());
			mSize[1] = Math::min(mSize[1] *= resizeScale.x(), targetSize.y());
		}
		else
		{
			//mSize[1] = Math::round(mSize[1] * resizeScale.y()); // this will be mTargetSize.y(). We can't exceed it.
			mSize[1] = mSize[1] * resizeScale.y(); // this will be mTargetSize.y(). We can't exceed it.

			// for SVG rasterization, always calculate width from rounded height (see comment above)
			// we need to make sure we're not creating an image larger than max size
			mSize[0] = Math::min((mSize[1] / textureSize.y()) * textureSize.x(), targetSize.x());
		}
	}
	else if (mTargetIsMin)
	{
		// mSize = ImageIO::getPictureMinSize(textureSize, mTargetSize);			
		mSize = textureSize;

		Vector2f resizeScale((targetSize.x() / mSize.x()), (targetSize.y() / mSize.y()));

		if (resizeScale.x() > resizeScale.y())
		{
			mSize[0] *= resizeScale.x();
			mSize[1] *= resizeScale.x();

			float cropPercent = (mSize.y() - targetSize.y()) / (mSize.y() * 2);
			crop(0, cropPercent, 0, cropPercent);
		}
		else
		{
			mSize[0] *= resizeScale.y();
			mSize[1] *= resizeScale.y();

			float cropPercent = (mSize.x() - targetSize.x()) / (mSize.x() * 2);
			crop(cropPercent, 0, cropPercent, 0);
		}

		// for SVG rasterization, always calculate width from rounded height (see comment above)
		// we need to make sure we're not creating an image smaller than min size
		// mSize[1] = Math::max(Math::round(mSize[1]), mTargetSize.y());
		// mSize[0] = Math::max((mSize[1] / textureSize.y()) * textureSize.x(), mTargetSize.x());
	}
	else
	{
		crop(0, 0, 0, 0);
		// if both components are set, we just stretch
		// if no components are set, we don't resize at all
		mSize = targetSize == Vector2f::Zero() ? textureSize : targetSize;

		// if only one component is set, we resize in a way that maintains aspect ratio
		// for SVG rasterization, we always calculate width from rounded height (see comment above)
		if (!targetSize.x() && targetSize.y())
		{
			//mSize[1] = Math::round(mTargetSize.y());
			mSize[1] = targetSize.y();
			mSize[0] = (mSize.y() / textureSize.y()) * textureSize.x();
		}
		else if (targetSize.x() && !targetSize.y())
		{
			//mSize[1] = Math::round((mTargetSize.x() / textureSize.x()) * textureSize.y());
			mSize[1] = (targetSize.x() / textureSize.x()) * textureSize.y();
			mSize[0] = (mSize.y() / textureSize.y()) * textureSize.x();
		}
	}

	mTexture->rasterizeAt((size_t)Math::round(mSize.x()), (size_t)Math::round(mSize.y()));
	onSizeChanged();
}

void VideoMpvComponent::onSizeChanged()
{
	GuiComponent::onSizeChanged();
	updateVertices();
}

void VideoMpvComponent::onPaddingChanged()
{
	GuiComponent::onPaddingChanged();
	resize();
	updateVertices();
}

void VideoMpvComponent::setColorShift(unsigned int color)
{
	mColorShift = color;
}

void VideoMpvComponent::updateVertices()
{
	if (!mTexture)
		return;

	auto textureSize = mTexture->getSize();

	Vector2f     topLeft = mSize * mTopLeftCrop;
	Vector2f     bottomRight = mSize * mBottomRightCrop;

	Vector2f paddingOffset;

	if (mPadding != Vector4f::Zero())
	{
		paddingOffset = mPadding.xy() - (mPadding.xy() + mPadding.zw()) * mOrigin;
		topLeft += paddingOffset;
		bottomRight += paddingOffset;
	}

	const float        px = mTexture->isTiled() ? mSize.x() / textureSize.x() : 1.0f;
	const float        py = mTexture->isTiled() ? mSize.y() / textureSize.y() : 1.0f;

	const unsigned int color = Renderer::convertColor(mColorShift);

	mVertices[0] = {
		{ topLeft.x(),					topLeft.y()	 },
		{ mTopLeftCrop.x(),				1.0f - mBottomRightCrop.y()    },
		color };

	mVertices[1] = {
		{ topLeft.x(),					bottomRight.y() },
		{ mTopLeftCrop.x(),				py - mTopLeftCrop.y() },
		color };

	mVertices[2] = {
		{ bottomRight.x(),				topLeft.y()	},
		{ mBottomRightCrop.x() * px,	1.0f - mBottomRightCrop.y()     },
		color };

	mVertices[3] = {
		{ bottomRight.x(),				bottomRight.y() },
		{ mBottomRightCrop.x() * px,    py - mTopLeftCrop.y() },
		color };

	// Fix vertices for min Target
	if (mTargetIsMin)
	{		
		auto targetSize = mTargetSize - mPadding.xy() - mPadding.zw();
		Vector2f targetSizePos = (mSize - targetSize) * mOrigin + paddingOffset;

		float x = targetSizePos.x();
		float y = targetSizePos.y();
		float r = x + targetSize.x();
		float b = y + targetSize.y();

		mVertices[0].pos[0] = x;
		mVertices[0].pos[1] = y;

		mVertices[1].pos[0] = x;
		mVertices[1].pos[1] = b;

		mVertices[2].pos[0] = r;
		mVertices[2].pos[1] = y;

		mVertices[3].pos[0] = r;
		mVertices[3].pos[1] = b;
	}

	/*
	// round vertices
	for (int i = 0; i < 4; ++i)
		mVertices[i].pos.round();
	*/
	/*
	if (mFlipX)
	{
		for (int i = 0; i < 4; ++i)
			mVertices[i].tex[0] = px - mVertices[i].tex[0];
	}

	if (mFlipY)
	{
		for (int i = 0; i < 4; ++i)
			mVertices[i].tex[1] = py - mVertices[i].tex[1];
	}
	*/
	updateColors();
	updateRoundCorners();	
}

void VideoMpvComponent::updateColors()
{
	float t = mFadeIn;
	if (mFadeIn < 1.0)
	{
		t = 1.0 - mFadeIn;
		t -= 1; // cubic ease in
		t = Math::lerp(0, 1, t * t * t + 1);
		t = 1.0 - t;
	}

	float opacity = (getOpacity() / 255.0f) * t;

	if (hasStoryBoard() && currentStoryBoardHasProperty("opacity") && isStoryBoardRunning())
		opacity = (getOpacity() / 255.0f);

	unsigned int color = Renderer::convertColor(mColorShift & 0xFFFFFF00 | (unsigned char)((mColorShift & 0xFF) * opacity));

	mVertices[0].col = color;
	mVertices[1].col = color;
	mVertices[2].col = color;
	mVertices[3].col = color;
}

void VideoMpvComponent::setRoundCorners(float value)
{
	if (mRoundCorners == value)
		return;

	VideoComponent::setRoundCorners(value);
	updateRoundCorners();
}

void VideoMpvComponent::updateRoundCorners()
{
	if (mRoundCorners <= 0 || Renderer::shaderSupportsCornerSize(mCustomShader.path))
	{
		mRoundCornerStencil.clear();
		return;
	}

	float x = 0;
	float y = 0;
	float size_x = mSize.x();
	float size_y = mSize.y();

	if (mTargetIsMin)
	{
		Vector2f targetSizePos = (mTargetSize - mSize) * mOrigin * -1;

		x = targetSizePos.x();
		y = targetSizePos.y();
		size_x = mTargetSize.x();
		size_y = mTargetSize.y();
	}

	float radius = mRoundCorners < 1 ? Math::max(size_x, size_y) * mRoundCorners : mRoundCorners;
	mRoundCornerStencil = Renderer::createRoundRect(x, y, size_x, size_y, radius);
}

void VideoMpvComponent::render(const Transform4x4f& parentTrans)
{
	if (!isShowing() || !isVisible())
		return;

	VideoComponent::render(parentTrans);

	bool initFromPixels = true;

	if (!mIsPlaying || !mContext || mIsParsing)
	{
		// If video is still attached to the path & texture is initialized, we suppose it had just been stopped (onhide, ondisable, screensaver...)
		// still render the last frame
		if (mTexture != nullptr && !mVideoPath.empty() && mPlayingVideoPath == mVideoPath && mTexture->isLoaded())
			initFromPixels = false;
		else
			return;
	}

	float t = mFadeIn;
	if (mFadeIn < 1.0)
	{
		t = 1.0 - mFadeIn;
		t -= 1; // cubic ease in
		t = Math::lerp(0, 1, t*t*t + 1);
		t = 1.0 - t;
	}

	if (t == 0.0)
		return;
		
	Transform4x4f trans = parentTrans * getTransform();
	
	if (mRotation == 0 && !mTargetIsMin)
	{
		auto rect = Renderer::getScreenRect(trans, mSize);
		if (!Renderer::isVisibleOnScreen(rect))
			return;
	}

	// Build a texture for the video frame
	if (initFromPixels)
	{		
		int frame = mContext->surfaceId;
		if (mContext->hasFrame[frame])
		{
			if (mTexture == nullptr)
			{
				mTexture = TextureResource::get("", false, mLinearSmooth);

				resize();
				trans = parentTrans * getTransform();
			}

#ifdef _RPI_
			// Rpi : A lot of videos are encoded in 60fps on screenscraper
			// Try to limit transfert to opengl textures to 30fps to save CPU
			if (!Settings::getInstance()->getBool("OptimizeVideo") || mElapsed >= 40) // 40ms = 25fps, 33.33 = 30 fps
#endif
			{
				mContext->mutexes[frame].lock();
				mTexture->updateFromExternalPixels(mContext->surfaces[frame], mVideoWidth, mVideoHeight);
				mContext->hasFrame[frame] = false;
				mContext->mutexes[frame].unlock();

				mElapsed = 0;
			}
		}
	}

	if (mTexture == nullptr)
		return;

	updateColors();

	bool isDefaultEffectDisabled = hasStoryBoard() && currentStoryBoardHasProperty("scale") && isStoryBoardRunning();

	/*if (mEffect == VideoMpvFlags::VideoMpvEffect::SLIDERIGHT && mFadeIn > 0.0 && mFadeIn < 1.0 && mConfig.startDelay > 0 && !isDefaultEffectDisabled)
	{
		float t = 1.0 - mFadeIn;
		t -= 1;
		t = Math::lerp(0, 1, t*t*t + 1);

		vertices[0] = { { 0.0f     , 0.0f      }, { t, 0.0f }, color };
		vertices[1] = { { 0.0f     , mSize.y() }, { t, 1.0f }, color };
		vertices[2] = { { mSize.x(), 0.0f      }, { t + 1.0f, 0.0f }, color };
		vertices[3] = { { mSize.x(), mSize.y() }, { t + 1.0f, 1.0f }, color };
	}
	else*/
	if (mEffect == VideoMpvFlags::VideoMpvEffect::SIZE && mFadeIn > 0.0 && mFadeIn < 1.0 && mConfig.startDelay > 0 && !isDefaultEffectDisabled)
	{		
		float bump = Math::easeOutCubic(mFadeIn);

		auto scale = mScale;
		mScale = mScale * bump;
		mTransformDirty = true;
		trans = parentTrans * getTransform();
		mScale = scale;
		mTransformDirty = true;
	}
	else if (mEffect == VideoMpvFlags::VideoMpvEffect::BUMP && mFadeIn > 0.0 && mFadeIn < 1.0 && mConfig.startDelay > 0 && !isDefaultEffectDisabled)
	{
		float bump = sin((MATHPI / 2.0) * mFadeIn) + sin(MATHPI * mFadeIn) / 2.0;

		auto scale = mScale;
		mScale = mScale * bump;
		mTransformDirty = true;
		trans = parentTrans * getTransform();
		mScale = scale;
		mTransformDirty = true;
	}

	// round vertices
	// for (int i = 0; i < 4; ++i)
	//	vertices[i].pos.round();
	
	if (mTexture->bind())
	{
		Renderer::setMatrix(trans);

		beginCustomClipRect();

		Vector2f targetSizePos = (mTargetSize - mSize) * mOrigin * -1;
		
		// Render it
		mVertices->saturation = mSaturation;
		mVertices->customShader = mCustomShader.path.empty() ? nullptr : &mCustomShader;
	
		if (mRoundCorners > 0 && mRoundCornerStencil.size() > 0)
		{
			Renderer::setStencil(mRoundCornerStencil.data(), mRoundCornerStencil.size());
			Renderer::drawTriangleStrips(&mVertices[0], 4);
			Renderer::disableStencil();
		}
		else
		{
			mVertices->cornerRadius = mRoundCorners < 1 ? Math::max(mSize.x(), mSize.y()) * mRoundCorners : mRoundCorners;
			Renderer::drawTriangleStrips(&mVertices[0], 4);
		}

		endCustomClipRect();

		Renderer::bindTexture(0);
	}
}

VideoContext* VideoMpvComponent::createContext()
{
	// Create an RGBA surface to render the video into
	VideoContext* ctx = new VideoContext();
	ctx->surfaces[0] = new unsigned char[mVideoWidth * mVideoHeight * 4];
	ctx->surfaces[1] = new unsigned char[mVideoWidth * mVideoHeight * 4];
	ctx->hasFrame[0] = false;
	ctx->hasFrame[1] = false;
	ctx->component = this;

	resize();	

	return ctx;
}

// Opens an mpv handle configured for rendering into our own buffer: no window,
// no terminal, no user config, and software decoding because the sw render API
// needs frames the CPU can read.
bool VideoMpvComponent::openHandle()
{
	mMpv = mpv_create();
	if (mMpv == nullptr)
		return false;

	mpv_set_option_string(mMpv, "config", "no");
	mpv_set_option_string(mMpv, "terminal", "no");
	mpv_set_option_string(mMpv, "msg-level", "all=no");
	mpv_set_option_string(mMpv, "input-default-bindings", "no");
	mpv_set_option_string(mMpv, "input-vo-keyboard", "no");
	mpv_set_option_string(mMpv, "osc", "no");
	mpv_set_option_string(mMpv, "osd-level", "0");
	mpv_set_option_string(mMpv, "vo", "libmpv");
	mpv_set_option_string(mMpv, "hwdec", "no");
	mpv_set_option_string(mMpv, "audio-display", "no");
	// We count loops ourselves, and keep-open leaves the file loaded at the end
	// so eof-reached can be read rather than the handle tearing itself down.
	mpv_set_option_string(mMpv, "loop-file", "no");
	mpv_set_option_string(mMpv, "keep-open", "yes");

	std::string options = SystemConf::getInstance()->get("mpv.options");
	if (!options.empty())
	{
		for (auto token : Utils::String::split(options, ' '))
		{
			auto eq = token.find('=');
			if (eq == std::string::npos)
				continue;

			auto key = Utils::String::replace(token.substr(0, eq), "--", "");
			mpv_set_option_string(mMpv, key.c_str(), token.substr(eq + 1).c_str());
		}
	}

	// Most videos have a fader, so a playlist skips the first second of it.
	if (mPlaylist != nullptr && mConfig.startDelay == 0 && !mConfig.showSnapshotDelay && !mConfig.showSnapshotNoVideo)
		mpv_set_option_string(mMpv, "start", "0.7");

	if (mpv_initialize(mMpv) < 0)
	{
		mpv_terminate_destroy(mMpv);
		mMpv = nullptr;
		return false;
	}

	return true;
}

void VideoMpvComponent::applyMute()
{
	if (mMpv == nullptr)
		return;

	bool mute = !getPlayAudio()
		|| (!mScreensaverMode && !Settings::getInstance()->getBool("VideoAudio"))
		|| (Settings::getInstance()->getBool("ScreenSaverVideoMute") && mScreensaverMode);

	int flag = mute ? 1 : 0;
	mpv_set_property(mMpv, "mute", MPV_FORMAT_FLAG, &flag);
}

void VideoMpvComponent::handleLooping()
{
	if (mIsPlaying && mMpv && !mIsParsing)
	{
		int eof = 0;
		if (mpv_get_property(mMpv, "eof-reached", MPV_FORMAT_FLAG, &eof) < 0)
			eof = 0;

		if (eof || mReachedEnd)
		{
			if (mLoops >= 0)
			{
				mCurrentLoop++;
				if (mCurrentLoop > mLoops)
				{
					stopVideo();

					mFadeIn = 0.0;
					mPlayingVideoPath = "";
					mVideoPath = "";
					return;
				}
			}

			if (mPlaylist != nullptr)
			{
				auto nextVideo = mPlaylist->getNextItem();
				if (!nextVideo.empty())
				{
					stopVideo();
					setVideo(nextVideo);
					return;
				}
				else
					mPlaylist = nullptr;
			}
			
			if (mVideoEnded != nullptr)
			{
				bool cont = mVideoEnded();
				if (!cont)
				{
					stopVideo();
					return;
				}
			}

			applyMute();

			mReachedEnd = false;

			const char* seek[] = { "seek", "0", "absolute", nullptr };
			mpv_command(mMpv, seek);

			int pause = 0;
			mpv_set_property(mMpv, "pause", MPV_FORMAT_FLAG, &pause);
		}
	}
}

void VideoMpvComponent::onMediaParsed()
{
	StopWatch stopWatch("[VideoMpvComponent] onMediaParsed", LogDebug);

	mVideoWidth = 0;
	mVideoHeight = 0;

	bool hasAudioTrack = false;

	char* aid = mpv_get_property_string(mMpv, "aid");
	if (aid != nullptr)
	{
		hasAudioTrack = strcmp(aid, "no") != 0 && aid[0] != 0;
		mpv_free(aid);
	}

	int64_t w = 0, h = 0;
	if (mpv_get_property(mMpv, "dwidth", MPV_FORMAT_INT64, &w) >= 0 &&
		mpv_get_property(mMpv, "dheight", MPV_FORMAT_INT64, &h) >= 0)
	{
		mVideoWidth = (unsigned int)w;
		mVideoHeight = (unsigned int)h;
	}

	mHasAudioTrack = hasAudioTrack;

	if (mVideoWidth == 0 && mVideoHeight == 0 && Utils::FileSystem::isAudio(mPlayingVideoPath))
	{
		if (getPlayAudio() && !mScreensaverMode && Settings::getInstance()->getBool("VideoAudio"))
		{
			// Make fake dimension to play audio files
			mVideoWidth = 1;
			mVideoHeight = 1;
		}
	}

	// Make sure we found a valid video track
	if (mVideoWidth <= 0 || mVideoHeight <= 0)
		return;

	if (mVideoWidth > 1 && Settings::getInstance()->getBool("OptimizeVideo"))
	{
		// Avoid videos bigger than resolution
		Vector2f maxSize(Renderer::getScreenWidth(), Renderer::getScreenHeight());

#ifdef _RPI_
		// Temporary -> RPI -> Try to limit videos to 400x300 for performance benchmark
		if (!Renderer::isSmallScreen())
			maxSize = Vector2f(400, 300);
#endif

		if (!mTargetSize.empty() && (mTargetSize.x() < maxSize.x() || mTargetSize.y() < maxSize.y()))
			maxSize = mTargetSize;

		// If video is bigger than display, render it smaller
		auto sz = ImageIO::adjustPictureSize(Vector2i(mVideoWidth, mVideoHeight), Vector2i(maxSize.x(), maxSize.y()), mTargetIsMin);
		if (sz.x() < mVideoWidth || sz.y() < mVideoHeight)
		{
			mVideoWidth = sz.x();
			mVideoHeight = sz.y();
		}
	}

	mContext = createContext();

	if (hasAudioTrack)
	{
		applyMute();

		if (getPlayAudio() && !(!mScreensaverMode && !Settings::getInstance()->getBool("VideoAudio")) && !(Settings::getInstance()->getBool("ScreenSaverVideoMute") && mScreensaverMode))
			AudioManager::setVideoPlaying(true);
	}

	// A width of 1 is the marker for an audio file being played for its sound
	// alone, so there is no point building a render context for it.
	if (mVideoWidth > 1)
	{
		mpv_render_param params[] = {
			{ MPV_RENDER_PARAM_API_TYPE, (void*)MPV_RENDER_API_TYPE_SW },
			{ MPV_RENDER_PARAM_INVALID, nullptr }
		};

		if (mpv_render_context_create(&mMpvRender, mMpv, params) < 0)
			mMpvRender = nullptr;
	}

	int pause = 0;
	mpv_set_property(mMpv, "pause", MPV_FORMAT_FLAG, &pause);
}

// One frame out of mpv and into the back surface. mpv writes the padding byte
// of rgb0 as zero, which our textures read as a transparent alpha, so it is
// forced opaque on the way past.
void VideoMpvComponent::readFrame()
{
	if (mMpvRender == nullptr || mContext == nullptr || mVideoWidth <= 1)
		return;

	if (!(mpv_render_context_update(mMpvRender) & MPV_RENDER_UPDATE_FRAME))
		return;

	int frame = (mContext->surfaceId ^ 1);

	int size[2] = { (int)mVideoWidth, (int)mVideoHeight };
	size_t stride = (size_t)mVideoWidth * 4;
	const char* format = "rgb0";

	mpv_render_param params[] = {
		{ MPV_RENDER_PARAM_SW_SIZE, size },
		{ MPV_RENDER_PARAM_SW_FORMAT, (void*)format },
		{ MPV_RENDER_PARAM_SW_STRIDE, &stride },
		{ MPV_RENDER_PARAM_SW_POINTER, mContext->surfaces[frame] },
		{ MPV_RENDER_PARAM_INVALID, nullptr }
	};

	if (mpv_render_context_render(mMpvRender, params) < 0)
		return;

	uint32_t* px = (uint32_t*)mContext->surfaces[frame];
	for (size_t i = 0, n = (size_t)mVideoWidth * mVideoHeight; i < n; i++)
		px[i] |= 0xFF000000;

	mContext->surfaceId = frame;
	mContext->hasFrame[frame] = true;

	if (!isPlaying() && isWaitingForVideoToStart())
		onVideoStarted();
}

void VideoMpvComponent::startVideo()
{
	if (!Settings::ShowVideoPreviews())
		return;

	if (mIsPlaying || mMpv != nullptr)
		return;

	if (mVideoPath.empty())
	{
		stopVideo();
		return;
	}

	StopWatch stopWatch("[VideoMpvComponent] startVideo", LogDebug);

#ifdef WIN32
	std::string path = Utils::String::replace(mVideoPath, "/", "\\");
#else
	std::string path = mVideoPath;
#endif

	if (!openHandle())
	{
		stopVideo();
		return;
	}

	if (hasStoryBoard("", true) && mConfig.startDelay > 0)
		startStoryboard();

	mTexture = nullptr;
	mCurrentLoop = 0;
	mReachedEnd = false;
	mHasAudioTrack = false;
	mPlayingVideoPath = mVideoPath;

	PowerSaver::pause();

	applyMute();

	const char* cmd[] = { "loadfile", path.c_str(), nullptr };
	if (mpv_command(mMpv, cmd) < 0)
	{
		stopVideo();
		return;
	}

	// Dimensions are only known once mpv has opened the file, so the rest of
	// the setup waits for MPV_EVENT_FILE_LOADED in update().
	mIsParsing = true;
}

void VideoMpvComponent::stopVideo()
{
	if (mMpv == nullptr && !mContext)
		return;

	StopWatch stopWatch("[VideoMpvComponent] stopVideo", LogDebug);

	mIsPlaying = false;
	mIsWaitingForVideoToStart = false;
	mStartDelayed = false;

	mIsParsing = false;
	mReachedEnd = false;

	// The render context has to go before the handle it belongs to, and it is
	// cheap, so it is freed here rather than on the release thread.
	if (mMpvRender)
	{
		mpv_render_context_free(mMpvRender);
		mMpvRender = nullptr;
	}

	if (mMpv)
	{
		mpv_release_async(mContext, mMpv);
		mMpv = nullptr;

		PowerSaver::resume();
	}
	else if (mContext)
		delete mContext;

	mContext = nullptr;

	if (mIsTopWindow) // Release texture memory -> except if mDisable by topWindow ( ex: menu was poped )
		mTexture = nullptr;

	AudioManager::setVideoPlaying(false);
}

void VideoMpvComponent::applyTheme(const std::shared_ptr<ThemeData>& theme, const std::string& view, const std::string& element, unsigned int properties)
{
	using namespace ThemeFlags;

	const ThemeData::ThemeElement* elem = theme->getElement(view, element, "video");
	if (!elem)
		return;

	if (elem && elem->has("effect"))
	{
		if (!(elem->get<std::string>("effect").compare("slideRight")))
			mEffect = VideoMpvFlags::VideoMpvEffect::SLIDERIGHT;
		else if (!(elem->get<std::string>("effect").compare("size")))
			mEffect = VideoMpvFlags::VideoMpvEffect::SIZE;
		else if (!(elem->get<std::string>("effect").compare("bump")))
			mEffect = VideoMpvFlags::VideoMpvEffect::BUMP;
		else
			mEffect = VideoMpvFlags::VideoMpvEffect::NONE;

		mConfig.scaleSnapshot = (mEffect != VideoMpvFlags::VideoMpvEffect::NONE);
	}

	if (elem && elem->has("roundCorners"))
		setRoundCorners(elem->get<float>("roundCorners"));
	
	if (properties & COLOR)
	{
		if (elem && elem->has("color"))
			setColorShift(elem->get<unsigned int>("color"));

		if (elem->has("linearSmooth"))
			mLinearSmooth = elem->get<bool>("linearSmooth");

		if (elem->has("saturation"))
			setSaturation(Math::clamp(elem->get<float>("saturation"), 0.0f, 1.0f));

		if (ThemeData::parseCustomShader(elem, &mCustomShader))
			updateRoundCorners();

		mStaticImage.setCustomShader(mCustomShader);
	}

	if (elem && elem->has("loops"))
		mLoops = (int)elem->get<float>("loops");
	else
		mLoops = -1;

	VideoComponent::applyTheme(theme, view, element, properties);
}

void VideoMpvComponent::update(int deltaTime)
{
	mElapsed += deltaTime;

	if (mConfig.showSnapshotNoVideo || mConfig.showSnapshotDelay)
		mStaticImage.update(deltaTime);

	// mpv is polled rather than pushing at us, so both the events and the frames
	// are picked up here, on the thread that owns the surfaces.
	if (mMpv != nullptr)
	{
		while (true)
		{
			mpv_event* event = mpv_wait_event(mMpv, 0);
			if (event == nullptr || event->event_id == MPV_EVENT_NONE)
				break;

			if (event->event_id == MPV_EVENT_FILE_LOADED && mIsParsing)
			{
				mIsParsing = false;
				onMediaParsed();
			}
			else if (event->event_id == MPV_EVENT_END_FILE)
				mReachedEnd = true;
			else if (event->event_id == MPV_EVENT_SHUTDOWN)
				break;
		}

		readFrame();
	}

	VideoComponent::update(deltaTime);
}

void VideoMpvComponent::onShow()
{
	VideoComponent::onShow();
	mStaticImage.onShow();

	if (hasStoryBoard("", true) && mConfig.startDelay > 0)
		pauseStoryboard();
}

ThemeData::ThemeElement::Property VideoMpvComponent::getProperty(const std::string name)
{
	Vector2f scale = getParent() ? getParent()->getSize() : Vector2f((float)Renderer::getScreenWidth(), (float)Renderer::getScreenHeight());
	
	if (Utils::String::startsWith(name, "shader."))
	{
		auto prop = name.substr(7);

		auto it = mCustomShader.parameters.find(prop);
		if (it != mCustomShader.parameters.cend())
			return Utils::String::toFloat(it->second);

		return 0.0f;
	}

	if (name == "size" || name == "maxSize" || name == "minSize")
		return mSize / scale;

	if (name == "color")
		return mColorShift;

	if (name == "roundCorners")
		return mRoundCorners;

	if (name == "saturation")
		return mSaturation;

	return VideoComponent::getProperty(name);
}

void VideoMpvComponent::setProperty(const std::string name, const ThemeData::ThemeElement::Property& value)
{
	Vector2f scale = getParent() ? getParent()->getSize() : Vector2f((float)Renderer::getScreenWidth(), (float)Renderer::getScreenHeight());
	
	if (value.type == ThemeData::ThemeElement::Property::PropertyType::Pair && (name == "maxSize" || name == "minSize"))
	{
		mSourceBounds.zw() = value.v;
		mTargetSize = Vector2f(value.v.x() * scale.x(), value.v.y() * scale.y());
		resize();
	}
	else if (value.type == ThemeData::ThemeElement::Property::PropertyType::Int && name == "color")
		setColorShift(value.i);
	else if (value.type == ThemeData::ThemeElement::Property::PropertyType::Float && name == "roundCorners")
		setRoundCorners(value.f);
	else if (value.type == ThemeData::ThemeElement::Property::PropertyType::Float && name == "saturation")
		setSaturation(value.f);
	else if (value.type == ThemeData::ThemeElement::Property::PropertyType::Float && Utils::String::startsWith(name, "shader."))
	{
		auto prop = name.substr(7);

		auto it = mCustomShader.parameters.find(prop);
		if (it != mCustomShader.parameters.cend())
			mCustomShader.parameters[prop] = std::to_string(value.f);
	}
	else 
		VideoComponent::setProperty(name, value);
}

void VideoMpvComponent::pauseVideo()
{
	if (!mIsPlaying && !mIsWaitingForVideoToStart && !mStartDelayed)
		return;

	mIsPlaying = false;
	mIsWaitingForVideoToStart = false;
	mStartDelayed = false;

	if (mMpv == NULL)
		stopVideo();
	else
	{
		int pause = 1;
		mpv_set_property(mMpv, "pause", MPV_FORMAT_FLAG, &pause);
		
		PowerSaver::resume();
		AudioManager::setVideoPlaying(false);
	}
}

void VideoMpvComponent::resumeVideo()
{
	if (mIsPlaying)
		return;

	if (mMpv == NULL)
	{
		startVideoWithDelay();
		return;
	}

	mIsPlaying = true;

	int pause = 0;
	mpv_set_property(mMpv, "pause", MPV_FORMAT_FLAG, &pause);
	PowerSaver::pause();
	AudioManager::setVideoPlaying(true);
}

bool VideoMpvComponent::isPaused()
{
	return !mIsPlaying && !mIsWaitingForVideoToStart && !mStartDelayed && mMpv != NULL;
}

void VideoMpvComponent::setSaturation(float saturation)
{
	mSaturation = saturation;
}
