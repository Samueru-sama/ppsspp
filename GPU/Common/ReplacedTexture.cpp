// Copyright (c) 2016- PPSSPP Project.

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0 or later versions.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License 2.0 for more details.

// A copy of the GPL 2.0 should have been included with the program.
// If not, see http://www.gnu.org/licenses/

// Official git repository and contact information can be found at
// https://github.com/hrydgard/ppsspp and http://www.ppsspp.org/.

#include "ppsspp_config.h"

#include <png.h>

#include "GPU/Common/ReplacedTexture.h"
#include "GPU/Common/TextureReplacer.h"

#include "Common/Data/Format/IniFile.h"
#include "Common/Data/Format/ZIMLoad.h"
#include "Common/Data/Format/PNGLoad.h"
#include "Common/Thread/ParallelLoop.h"
#include "Common/Thread/Waitable.h"
#include "Common/Thread/ThreadManager.h"
#include "Common/Log.h"
#include "Common/TimeUtil.h"

class ReplacedTextureTask : public Task {
public:
	ReplacedTextureTask(VFSBackend *vfs, ReplacedTexture &tex, LimitedWaitable *w) : vfs_(vfs), tex_(tex), waitable_(w) {}

	TaskType Type() const override {
		return TaskType::IO_BLOCKING;
	}

	TaskPriority Priority() const override {
		return TaskPriority::NORMAL;
	}

	void Run() override {
		tex_.Prepare(vfs_);
		waitable_->Notify();
	}

private:
	VFSBackend *vfs_;
	ReplacedTexture &tex_;
	LimitedWaitable *waitable_;
};

// This can only return true if ACTIVE or NOT_FOUND.
bool ReplacedTexture::IsReady(double budget) {
	_assert_(vfs_ != nullptr);

	switch (State()) {
	case ReplacementState::ACTIVE:
	case ReplacementState::NOT_FOUND:
		if (threadWaitable_) {
			if (!threadWaitable_->WaitFor(budget)) {
				return false;
			}
			// Successfully waited! Can get rid of it.
			threadWaitable_->WaitAndRelease();
			threadWaitable_ = nullptr;
		}
		lastUsed_ = time_now_d();
		return true;
	case ReplacementState::UNINITIALIZED:
		// _dbg_assert_(false);
		return false;
	case ReplacementState::CANCEL_INIT:
	case ReplacementState::PENDING:
		return false;
	case ReplacementState::POPULATED:
		// We're gonna need to spawn a task.
		break;
	}

	lastUsed_ = time_now_d();

	// Let's not even start a new texture if we're already behind.
	if (budget < 0.0)
		return false;

	_assert_(!threadWaitable_);
	threadWaitable_ = new LimitedWaitable();
	SetState(ReplacementState::PENDING);
	g_threadManager.EnqueueTask(new ReplacedTextureTask(vfs_, *this, threadWaitable_));
	if (threadWaitable_->WaitFor(budget)) {
		// If we successfully wait here, we're done. The thread will set state accordingly.
		_assert_(State() == ReplacementState::ACTIVE || State() == ReplacementState::NOT_FOUND || State() == ReplacementState::CANCEL_INIT);
		return true;
	}
	// Still pending on thread.
	return false;
}

void ReplacedTexture::FinishPopulate(ReplacementDesc *desc) {
	logId_ = desc->logId;
	levelData_ = desc->cache;
	desc_ = desc;
	SetState(ReplacementState::POPULATED);

	// TODO: What used to be here is now done on the thread task.
}

void ReplacedTexture::Prepare(VFSBackend *vfs) {
	this->vfs_ = vfs;

	std::unique_lock<std::mutex> lock(mutex_);

	_assert_msg_(levelData_ != nullptr, "Level cache not set");

	// We must lock around access to levelData_ in case two textures try to load it at once.
	std::lock_guard<std::mutex> guard(levelData_->lock);

	for (int i = 0; i < std::min(MAX_REPLACEMENT_MIP_LEVELS, (int)desc_->filenames.size()); ++i) {
		if (State() == ReplacementState::CANCEL_INIT) {
			break;
		}

		if (desc_->filenames[i].empty()) {
			// Out of valid mip levels.  Bail out.
			break;
		}

		const Path filename = desc_->basePath / desc_->filenames[i];

		VFSFileReference *fileRef = vfs_->GetFile(desc_->filenames[i].c_str());
		if (!fileRef) {
			// If the file doesn't exist, let's just bail immediately here.
			break;
		}

		// TODO: Here, if we find a file with multiple built-in mipmap levels,
		// we'll have to change a bit how things work...
		ReplacedTextureLevel level;
		level.file = filename;

		if (i == 0) {
			fmt = Draw::DataFormat::R8G8B8A8_UNORM;
		}

		level.fileRef = fileRef;

		if (LoadLevelData(level, i)) {
			levels_.push_back(level);
		} else {
			// Otherwise, we're done loading mips (bad PNG or bad size, either way.)
			break;
		}
	}

	delete desc_;
	desc_ = nullptr;

	if (levels_.empty()) {
		// Bad.
		SetState(ReplacementState::NOT_FOUND);
		levelData_ = nullptr;
		return;
	}

	SetState(ReplacementState::ACTIVE);

	if (threadWaitable_)
		threadWaitable_->Notify();
}

bool ReplacedTexture::LoadLevelData(ReplacedTextureLevel &level, int mipLevel) {
	bool good = false;

	size_t fileSize;
	VFSOpenFile *openFile = vfs_->OpenFileForRead(level.fileRef, &fileSize);
	if (!openFile) {
		return false;
	}

	std::string magic;
	ReplacedImageType imageType = Identify(vfs_, openFile, &magic);

	if (imageType == ReplacedImageType::ZIM) {
		uint32_t ignore = 0;
		struct ZimHeader {
			uint32_t magic;
			uint32_t w;
			uint32_t h;
			uint32_t flags;
		} header;
		good = vfs_->Read(openFile, &header, sizeof(header)) == sizeof(header);
		level.w = header.w;
		level.h = header.h;
		good = (header.flags & ZIM_FORMAT_MASK) == ZIM_RGBA8888;
	} else if (imageType == ReplacedImageType::PNG) {
		PNGHeaderPeek headerPeek;
		good = vfs_->Read(openFile, &headerPeek, sizeof(headerPeek)) == sizeof(headerPeek);
		if (good && headerPeek.IsValidPNGHeader()) {
			level.w = headerPeek.Width();
			level.h = headerPeek.Height();
			good = true;
		} else {
			ERROR_LOG(G3D, "Could not get PNG dimensions: %s (zip)", level.file.ToVisualString().c_str());
			good = false;
		}
	} else {
		ERROR_LOG(G3D, "Could not load texture replacement info: %s - unsupported format %s", level.file.ToVisualString().c_str(), magic.c_str());
	}

	// Is this really the right place to do it?
	level.w = (level.w * desc_->w) / desc_->newW;
	level.h = (level.h * desc_->h) / desc_->newH;

	if (good && mipLevel != 0) {
		// Check that the mipmap size is correct.  Can't load mips of the wrong size.
		if (level.w != (levels_[0].w >> mipLevel) || level.h != (levels_[0].h >> mipLevel)) {
			WARN_LOG(G3D, "Replacement mipmap invalid: size=%dx%d, expected=%dx%d (level %d)",
				level.w, level.h, levels_[0].w >> mipLevel, levels_[0].h >> mipLevel, mipLevel);
			good = false;
		}
	}

	if (!good) {
		return false;
	}

	if (levelData_->data.size() <= mipLevel) {
		levelData_->data.resize(mipLevel + 1);
	}

	std::vector<uint8_t> &out = levelData_->data[mipLevel];

	// Already populated from cache.
	if (!out.empty()) {
		return true;
	}

	auto cleanup = [&] {
		vfs_->CloseFile(openFile);
	};

	vfs_->Rewind(openFile);

	if (imageType == ReplacedImageType::ZIM) {
		std::unique_ptr<uint8_t[]> zim(new uint8_t[fileSize]);
		if (!zim) {
			ERROR_LOG(G3D, "Failed to allocate memory for texture replacement");
			SetState(ReplacementState::NOT_FOUND);
			cleanup();
			return false;
		}

		if (vfs_->Read(openFile, &zim[0], fileSize) != fileSize) {
			ERROR_LOG(G3D, "Could not load texture replacement: %s - failed to read ZIM", level.file.c_str());
			SetState(ReplacementState::NOT_FOUND);
			cleanup();
			return false;
		}

		int w, h, f;
		uint8_t *image;
		if (LoadZIMPtr(&zim[0], fileSize, &w, &h, &f, &image)) {
			if (w > level.w || h > level.h) {
				ERROR_LOG(G3D, "Texture replacement changed since header read: %s", level.file.c_str());
				SetState(ReplacementState::NOT_FOUND);
				cleanup();
				return false;
			}

			out.resize(level.w * level.h * 4);
			if (w == level.w) {
				memcpy(&out[0], image, level.w * 4 * level.h);
			} else {
				for (int y = 0; y < h; ++y) {
					memcpy(&out[level.w * 4 * y], image + w * 4 * y, w * 4);
				}
			}
			free(image);
		}

		CheckAlphaResult res = CheckAlpha32Rect((u32 *)&out[0], level.w, w, h, 0xFF000000);
		if (res == CHECKALPHA_ANY || mipLevel == 0) {
			alphaStatus_ = ReplacedTextureAlpha(res);
		}
	} else if (imageType == ReplacedImageType::PNG) {
		png_image png = {};
		png.version = PNG_IMAGE_VERSION;

		std::string pngdata;
		pngdata.resize(fileSize);
		pngdata.resize(vfs_->Read(openFile, &pngdata[0], fileSize));
		if (!png_image_begin_read_from_memory(&png, &pngdata[0], pngdata.size())) {
			ERROR_LOG(G3D, "Could not load texture replacement info: %s - %s (zip)", level.file.c_str(), png.message);
			SetState(ReplacementState::NOT_FOUND);
			cleanup();
			return false;
		}
		if (png.width > (uint32_t)level.w || png.height > (uint32_t)level.h) {
			ERROR_LOG(G3D, "Texture replacement changed since header read: %s", level.file.c_str());
			SetState(ReplacementState::NOT_FOUND);
			cleanup();
			return false;
		}

		bool checkedAlpha = false;
		if ((png.format & PNG_FORMAT_FLAG_ALPHA) == 0) {
			// Well, we know for sure it doesn't have alpha.
			if (mipLevel == 0) {
				alphaStatus_ = ReplacedTextureAlpha::FULL;
			}
			checkedAlpha = true;
		}
		png.format = PNG_FORMAT_RGBA;

		out.resize(level.w * level.h * 4);
		if (!png_image_finish_read(&png, nullptr, &out[0], level.w * 4, nullptr)) {
			ERROR_LOG(G3D, "Could not load texture replacement: %s - %s", level.file.c_str(), png.message);
			SetState(ReplacementState::NOT_FOUND);
			cleanup();
			out.resize(0);
			return false;
		}
		png_image_free(&png);

		if (!checkedAlpha) {
			// This will only check the hashed bits.
			CheckAlphaResult res = CheckAlpha32Rect((u32 *)&out[0], level.w, png.width, png.height, 0xFF000000);
			if (res == CHECKALPHA_ANY || mipLevel == 0) {
				alphaStatus_ = ReplacedTextureAlpha(res);
			}
		}
	}

	cleanup();
	return true;
}

void ReplacedTexture::PurgeIfOlder(double t) {
	if (threadWaitable_ && !threadWaitable_->WaitFor(0.0))
		return;
	if (lastUsed_ >= t)
		return;

	if (levelData_->lastUsed < t) {
		// We have to lock since multiple textures might reference this same data.
		std::lock_guard<std::mutex> guard(levelData_->lock);
		levelData_->data.clear();
		// This means we have to reload.  If we never purge any, there's no need.
		SetState(ReplacementState::POPULATED);
	}
}

ReplacedTexture::~ReplacedTexture() {
	if (threadWaitable_) {
		SetState(ReplacementState::CANCEL_INIT);

		std::unique_lock<std::mutex> lock(mutex_);
		threadWaitable_->WaitAndRelease();
		threadWaitable_ = nullptr;
	}

	for (auto &level : levels_) {
		vfs_->ReleaseFile(level.fileRef);
		level.fileRef = nullptr;
	}
}

bool ReplacedTexture::CopyLevelTo(int level, void *out, int rowPitch) {
	_assert_msg_((size_t)level < levels_.size(), "Invalid miplevel");
	_assert_msg_(out != nullptr && rowPitch > 0, "Invalid out/pitch");

	if (State() != ReplacementState::ACTIVE) {
		WARN_LOG(G3D, "Init not done yet");
		return false;
	}

	// We probably could avoid this lock, but better to play it safe.
	std::lock_guard<std::mutex> guard(levelData_->lock);

	const ReplacedTextureLevel &info = levels_[level];
	const std::vector<uint8_t> &data = levelData_->data[level];

	if (data.empty()) {
		WARN_LOG(G3D, "Level %d is empty", level);
		return false;
	}

	if (rowPitch < info.w * 4) {
		ERROR_LOG(G3D, "Replacement rowPitch=%d, but w=%d (level=%d)", rowPitch, info.w * 4, level);
		return false;
	}
	_assert_msg_(data.size() == info.w * info.h * 4, "Data has wrong size");

	if (rowPitch == info.w * 4) {
		ParallelMemcpy(&g_threadManager, out, &data[0], info.w * 4 * info.h);
	} else {
		const int MIN_LINES_PER_THREAD = 4;
		ParallelRangeLoop(&g_threadManager, [&](int l, int h) {
			for (int y = l; y < h; ++y) {
				memcpy((uint8_t *)out + rowPitch * y, &data[0] + info.w * 4 * y, info.w * 4);
			}
			}, 0, info.h, MIN_LINES_PER_THREAD);
	}

	return true;
}

const char *StateString(ReplacementState state) {
	switch (state) {
	case ReplacementState::UNINITIALIZED: return "UNINITIALIZED";
	case ReplacementState::POPULATED: return "PREPARED";
	case ReplacementState::PENDING: return "PENDING";
	case ReplacementState::NOT_FOUND: return "NOT_FOUND";
	case ReplacementState::ACTIVE: return "ACTIVE";
	case ReplacementState::CANCEL_INIT: return "CANCEL_INIT";
	default: return "N/A";
	}
}