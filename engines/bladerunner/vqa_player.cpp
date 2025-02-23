/* ScummVM - Graphic Adventure Engine
 *
 * ScummVM is the legal property of its developers, whose names
 * are too numerous to list here. Please refer to the COPYRIGHT
 * file distributed with this source distribution.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 */

#include "bladerunner/vqa_player.h"

#include "bladerunner/bladerunner.h"
#include "bladerunner/time.h"
#include "bladerunner/audio_player.h"

#include "audio/decoders/raw.h"

#include "common/system.h"

#include "common/rect.h"
#include "image/png.h"

using namespace std;

namespace BladeRunner {

bool VQAPlayer::open() {
	_s = _vm->getResourceStream(_name);
	if (!_s) {
		return false;
	}

	if (!_decoder.loadStream(_s)) {
		delete _s;
		_s = nullptr;
		return false;
	}

#if !BLADERUNNER_ORIGINAL_BUGS
	// TB05 has wrong end of a loop and this will load empty zbuffer from next loop, which will lead to broken pathfinding
	if (_name.equals("TB05_2.VQA")) {
		_decoder._loopInfo.loops[1].end = 60;
	} else if (_name.equals("DR04OVER.VQA")) {
		// smoke (overlay) after explosion of Dermo Labs in DR04
		// This has still frames in the end that so it looked as if the smoke was "frozen"
		_decoder._loopInfo.loops[0].end  = 58; // 59 up to 74 are still frames
	}
#endif

	_hasAudio = _decoder.hasAudio();
	if (_hasAudio) {
		_audioStream = Audio::makeQueuingAudioStream(_decoder.frequency(), false);
		_lastAudioFrameSuccessfullyQueued = 1;
	}

	_repeatsCount = 0;
	_loopNext = -1;
	_frame = -1;
	_frameBeginNext = -1;
	_frameEnd = getFrameCount() - 1;
	_frameEndQueued = -1;
	_repeatsCountQueued = -1;

	if (_loopInitial >= 0) {
		// loopInitial is set to the loop Id value that should play,
		// when the SeekableReadStream (_s) is nullptr
		// see setLoop()
		setLoop(_loopInitial, _repeatsCountInitial, kLoopSetModeImmediate, nullptr, nullptr);
	} else {
		_frameNext = 0;
		// TODO? Removed as redundant
//		setBeginAndEndFrame(0, _frameEnd, 0, kLoopSetModeJustStart, nullptr, nullptr);
	}

	return true;
}

void VQAPlayer::close() {
	_vm->_mixer->stopHandle(_soundHandle);
	delete _s;
	_s = nullptr;
}

int VQAPlayer::update(bool forceDraw, bool advanceFrame, bool useTime, Graphics::Surface *customSurface) {
	uint32 now = 60 * _vm->_time->currentSystem();
	int result = -1;

	if (_frameNext < 0) {
		_frameNext = _frameBeginNext;
	}

	if ((_repeatsCount > 0 || _repeatsCount == -1) && (_frameNext > _frameEnd)) {
		int loopEndQueued = _frameEndQueued;
		if (_frameEndQueued != -1) {
			_frameEnd = _frameEndQueued;
			_frameEndQueued = -1;
#if !BLADERUNNER_ORIGINAL_BUGS
			// Fix glitch in transition from inShot to mainloop
			// in Act 5 at McCoy's apartment (moving from bedroom to balcony).
			// This emulates a fast-forward, which is required
			// in order to have proper z-buffer info,
			// and display the new first frame of the loop (60) without artifacts.
			// The code is similar to Scene::advanceFrame()
			// This will be done once, since this first loop (loopId 1)
			// is only executed once before moving on to loopId 2
			if (_name.equals("MA05_3.VQA") && _loopNext == 1) {
				while (update(false, true, false) != 59) {
					updateZBuffer(_vm->_zbuffer);
				}
				// This works because the loopId 1 executes once before moving to _loop 2
				// See Scene::advanceFrame()
				//     Scene::loopEnded()
				//
				_frameBeginNext = 60;
			}
#endif
		}
		if (_frameNext != _frameBeginNext) {
			_frameNext = _frameBeginNext;
		}

		if (loopEndQueued == -1) {
			if (_repeatsCount != -1) {
				--_repeatsCount;
			}
			//callback for repeat, it is not used in the blade runner
		} else {
			_repeatsCount = _repeatsCountQueued;
			_repeatsCountQueued = -1;

			if (_callbackLoopEnded != nullptr) {
				_callbackLoopEnded(_callbackData, 0, _loopNext);
			}
		}
		result = -1;
	} else if (_frameNext > _frameEnd) {
		result = -3;
		// _repeatsCount == 0, so return here at the end of the video, to release the resource
		return result;
	} else if (useTime && (now - (_frameNextTime - kVqaFrameTimeDiff) < kVqaFrameTimeDiff)) {
		// Not yet time to move to next frame.
		// Note, we use unsigned difference to avoid potential time overflow issues
		result = -1;
	} else if (advanceFrame) {
		Graphics::Surface *surface = customSurface != nullptr ? customSurface : _surface;


		_frame = _frameNext;
		_decoder.readFrame(_frameNext, kVQAReadVideo);


		Common::String name;

		Common::Path framefilename;

		Common::FSNode file;

		if (_decoder._header.width == 640 && _decoder._header.height == 480) {
			name = Common::String::format("%s_%04d.png", _name.c_str(), _frameNext);
		} else {
			name = Common::String::format("%s_%d-%d_%dx%d_a_%04d.png", _name.c_str(), _decoder._header.offsetX, _decoder._header.offsetY, _decoder._header.width, _decoder._header.height, _frameNext);

			surface->fillRect(Common::Rect(surface->w, surface->h), 65535);
		}


		_decoder.decodeVideoFrame(surface, _frameNext);


		framefilename = Common::Path(name);
		file = Common::FSNode(framefilename);
		if (!file.exists()) {

			//Common::SeekableWriteStream *stream = file.createWriteStream();				

			//bool writePNG(Common::WriteStream &out, const Graphics::Surface &input, const byte *palette) {


			Common::Rect r = Common::Rect(surface->w, surface->h);
			//Common::Rect r = Common::Rect(_decoder._header.width, _decoder._header.height, _decoder._header.offsetX, _decoder._header.offsetY);
			//r.left = 0;
			//Image::writePNG(stream->writeStream, _surface->getSubArea(r));
			Common::SeekableWriteStream *writer = file.createWriteStream();
			Image::writePNG(*writer, surface->getSubArea(r));
			writer->finalize();
			delete writer;
		}


		int maxAllowedAudioPreloadedFrames = kMaxAudioPreloadedFrames;
		if (_frameEnd - _frameNext < kMaxAudioPreloadedFrames - 1) {
			maxAllowedAudioPreloadedFrames = _frameEnd - _frameNext + 1;
		}

		if (_hasAudio) {
			if (!_audioStarted) {
				// start with preloading up to (kMaxAudioPreloadedFrames - 1) frames at most, before reaching the _frameEnd frame
				for (int i = 0; i < kMaxAudioPreloadedFrames - 1; ++i) {
					if (_frameNext + i < _frameEnd) {
						_decoder.readFrame(_frameNext + i, kVQAReadAudio);
						queueAudioFrame(_decoder.decodeAudioFrame());
						_lastAudioFrameSuccessfullyQueued = _frameNext + i;
					}
				}
				if (_vm->_mixer->isReady()) {
					// Audio stream starts playing, consuming queued "audio frames"
					// Note: On its own, the audio will not re-synch with video;
					// It plays independently so it can get ahead!
					_vm->_mixer->playStream(kVQASoundType, &_soundHandle, _audioStream);
				}
				_audioStarted = true;
			}

			// Due to our audio stream being queuable, the queued audio frames will play,
			// even if the game is "paused" eg. by moving the ScummVM window.
			// However, the video will stop playing immediately in that case.
			// That would result in a audio video desynch, with audio being ahead of video.
			// When the video resumes, we need to catch up to the audio "frame" of the queue that was last played,
			// without queuing more audio, and then start queuing audio again.

			// The following still covers the case of adding the final 15th audio frame to the queue
			// when first starting the audio stream.
			int tmpCurrentQueuedAudioFrames = getQueuedAudioFrames();
			if (_lastAudioFrameSuccessfullyQueued != _frameEnd) {
				// if video is behind audio, then resynch,
				// which here means: don't queue and don't play audio until video catches up.
			    if (_lastAudioFrameSuccessfullyQueued - tmpCurrentQueuedAudioFrames < _frameNext) {
					int addToQueueRep = 0;
					while (addToQueueRep < (maxAllowedAudioPreloadedFrames - tmpCurrentQueuedAudioFrames)
					       && _lastAudioFrameSuccessfullyQueued + 1 <= _frameEnd) {
						_decoder.readFrame(_lastAudioFrameSuccessfullyQueued + 1, kVQAReadAudio);
						queueAudioFrame(_decoder.decodeAudioFrame());
						++_lastAudioFrameSuccessfullyQueued;
						++addToQueueRep;
					}
				}
			}
		}

		if (useTime) {
			_frameNextTime += kVqaFrameTimeDiff;

			// In some cases (as overlay paused by kia or game window is moved) new time might be still in the past.
			// This can cause rapid playback of video where every refresh renders different frame of the video.
			// Can be avoided by setting next time to the future.
			// Note, we use unsigned difference to avoid time overflow issues
			if (now - (_frameNextTime - kVqaFrameTimeDiff) > kVqaFrameTimeDiff) {
				_frameNextTime = now + kVqaFrameTimeDiff;
			}
		}
		++_frameNext;
		result = _frame;
	}

	if (result < 0 && forceDraw && _frame != -1) {
		_decoder.decodeVideoFrame(customSurface != nullptr ? customSurface : _surface, _frame, true);
		result = _frame;
	}
	return result; // Note: result here could be negative.
	               // Negative valid value should only be -1, since there are various assertions
	               // assert(frame >= -1) in overlay modes (elevator, scores, spinner)
}

void VQAPlayer::updateZBuffer(ZBuffer *zbuffer) {
	_decoder.decodeZBuffer(zbuffer);
}

void VQAPlayer::updateView(View *view) {
	_decoder.decodeView(view);
}

void VQAPlayer::updateScreenEffects(ScreenEffects *screenEffects) {
	_decoder.decodeScreenEffects(screenEffects);
}

void VQAPlayer::updateLights(Lights *lights) {
	_decoder.decodeLights(lights);
}

bool VQAPlayer::setLoop(int loop, int repeatsCount, int loopSetMode, void (*callback)(void *, int, int), void *callbackData) {
	if (_s == nullptr) {
		_loopInitial = loop;
		_repeatsCountInitial = repeatsCount;
		return true;
	}

	int begin, end;
	if (!_decoder.getLoopBeginAndEndFrame(loop, &begin, &end)) {
		return false;
	}
	if (setBeginAndEndFrame(begin, end, repeatsCount, loopSetMode, callback, callbackData)) {
		_loopNext = loop;
		return true;
	}
	return false;
}

bool VQAPlayer::setBeginAndEndFrame(int begin, int end, int repeatsCount, int loopSetMode, void (*callback)(void *, int, int), void *callbackData) {
	if ( begin >= getFrameCount()
	    || end >= getFrameCount()
	    || begin >= end
	    || loopSetMode < 0
	    || loopSetMode >= 3
	) {
		warning("VQAPlayer::setBeginAndEndFrame - Invalid arguments for video");
		return false; // VQA_DECODER_ERROR_BAD_INPUT case
	}

	if (repeatsCount < 0) {
		repeatsCount = -1;
	}

	if (_repeatsCount == 0 && loopSetMode == kLoopSetModeEnqueue) {
		// if the member var _repeatsCount is 0 (which means "current playing loop will not be repeated")
		// then do not enqueue and, instead, treat the request as kLoopSetModeImmediate
		loopSetMode = kLoopSetModeImmediate;
	}

	_frameBeginNext = begin;

	if (loopSetMode == kLoopSetModeJustStart) {
		_repeatsCount = repeatsCount;
		_frameEnd = end;
	} else if (loopSetMode == kLoopSetModeEnqueue) {
		_repeatsCountQueued = repeatsCount;
		_frameEndQueued = end;
	} else if (loopSetMode == kLoopSetModeImmediate) {
		_repeatsCount = repeatsCount;
		_frameEnd = end;
		seekToFrame(begin);
	}

	_callbackLoopEnded = callback;
	_callbackData = callbackData;

	return true;
}

bool VQAPlayer::seekToFrame(int frame) {
	_frameNext = frame;
	_frameNextTime = 60 * _vm->_time->currentSystem();
	return true;
}

bool VQAPlayer::getCurrentBeginAndEndFrame(int frame, int *begin, int *end) {
	int playingLoop = _decoder.getLoopIdFromFrame(frame);
	if (playingLoop != -1) {
		return _decoder.getLoopBeginAndEndFrame(playingLoop, begin, end);
	}
	return false;
}

int VQAPlayer::getLoopBeginFrame(int loop) {
	int begin, end;
	if (!_decoder.getLoopBeginAndEndFrame(loop, &begin, &end)) {
		return -1;
	}
	return begin;
}

int VQAPlayer::getLoopEndFrame(int loop) {
	int begin, end;
	if (!_decoder.getLoopBeginAndEndFrame(loop, &begin, &end)) {
		return -1;
	}
	return end;
}

int VQAPlayer::getFrameCount() const {
	return _decoder.numFrames();
}

int VQAPlayer::getQueuedAudioFrames() const {
	return _audioStream->numQueuedStreams();
}

// Adds another audio "frame" to the queue of the audio stream
void VQAPlayer::queueAudioFrame(Audio::AudioStream *audioStream) {
	if (audioStream == nullptr) {
		return;
	}

	int n = _audioStream->numQueuedStreams();
	// TODO Maybe remove this warning or make it a debug-only message?
	if (n == 0) {
		warning("numQueuedStreams: %d", n);
	}

	_audioStream->queueAudioStream(audioStream, DisposeAfterUse::YES);
}

} // End of namespace BladeRunner
