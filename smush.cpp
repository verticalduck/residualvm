// Residual - Virtual machine to run LucasArts' 3D adventure games
// Copyright (C) 2003-2004 The ScummVM-Residual Team (www.scummvm.org)
//
//  This library is free software; you can redistribute it and/or
//  modify it under the terms of the GNU Lesser General Public
//  License as published by the Free Software Foundation; either
//  version 2.1 of the License, or (at your option) any later version.
//
//  This library is distributed in the hope that it will be useful,
//  but WITHOUT ANY WARRANTY; without even the implied warranty of
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
//  Lesser General Public License for more details.
//
//  You should have received a copy of the GNU Lesser General Public
//  License along with this library; if not, write to the Free Software
//  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307  USA

#include "stdafx.h"
#include "bits.h"
#include "debug.h"
#include <cstring>
#include <zlib.h>
#include "smush.h"
#include "timer.h"
#include "mixer/mixer.h"
#include "driver_gl.h"
#include "resource.h"

Smush *g_smush;

void Smush::timerCallback(void *refCon) {
	g_smush->handleFrame();
}

Smush::Smush() {
	g_smush = this;
	_nbframes = 0;
	_dst = NULL;
	_width = 0;
	_height = 0;
	_speed = 0;
	_channels = -1;
	_freq = 22050;
	_videoFinished = false;
	_videoPause = true;
	_movieTime = 0;
}

Smush::~Smush() {
	deinit();
}

void Smush::init() {
	_frame = 0;
	_movieTime = 0;
	_videoFinished = false;
	_videoPause = false;
	g_timer->installTimerProc(&timerCallback, _speed, NULL);
}

void Smush::deinit() {
	_videoFinished = true;
	_videoPause = true;
	g_timer->removeTimerProc(&timerCallback);
	_file.close();
}

void Smush::handleBlocky16(byte *src) {
	_blocky16.decode(_dst, src);
}

static uint16 destTable[5786];
void vimaInit(uint16 *destTable);
void decompressVima(const char *src, int16 *dest, int destLen, uint16 *destTable);

extern SoundMixer *g_mixer;

void Smush::handleWave(const byte *src, uint32 size) {
	int16 *dst = new int16[size * _channels];
	decompressVima((char *)src, dst, size * _channels * 2, destTable);

	for (uint32 j = 0; j < size * _channels; j++)
		dst[j] = SWAP_BYTES_16(dst[j]);
 
	int flags = SoundMixer::FLAG_16BITS | SoundMixer::FLAG_AUTOFREE;
//	int flags = SoundMixer::FLAG_16BITS | SoundMixer::FLAG_AUTOFREE | SoundMixer::FLAG_LITTLE_ENDIAN;
	if (_channels == 2)
		flags |= SoundMixer::FLAG_STEREO;

	if (!_soundHandle.isActive())
		g_mixer->newStream(&_soundHandle, _freq, flags, 500000);
	g_mixer->appendStream(_soundHandle, (byte *)dst, size * _channels * 2);
}

void Smush::handleFrame() {
	uint32 tag;
	int32 size;
	int pos = 0;

	if (_videoPause)
		return;

	if (_videoFinished) {
		_videoPause = true;
		return;
	}

	tag = _file.readUint32BE();
	if (tag == MKID_BE('ANNO')) {
		size = _file.readUint32BE();
		_file.seek(size, SEEK_CUR);
		tag = _file.readUint32BE();
	}
	assert(tag == MKID_BE('FRME'));
	size = _file.readUint32BE();
	byte *frame = (byte*)malloc(size);
	_file.read(frame, size);
	do {
		if (READ_BE_UINT32(frame + pos) == MKID_BE('Bl16')) {
			handleBlocky16(frame + pos + 8);
			pos += READ_BE_UINT32(frame + pos + 4) + 8;
		} else if (READ_BE_UINT32(frame + pos) == MKID_BE('Wave')) {
			int decompressed_size = READ_BE_UINT32(frame + pos + 8);
			if (decompressed_size < 0)
				handleWave(frame + pos + 8 + 4 + 8, READ_BE_UINT32(frame + pos + 8 + 8));
			else
				handleWave(frame + pos + 8 + 4, decompressed_size);
			pos += READ_BE_UINT32(frame + pos + 4) + 8;
		} else {
			error("unknown tag");
		}
	} while (pos < size);
	free(frame);

	memcpy(_buf, _dst, _width * _height * 2);
	_updateNeeded = true;

	_frame++;
	if (_frame == _nbframes) {
		_videoFinished = true;
	}
	
	_movieTime += _speed / 1000;
}

void Smush::handleFramesHeader() {
	uint32 tag;
	int32 size;
	int pos = 0;

	tag = _file.readUint32BE();
	assert(tag == MKID_BE('FLHD'));
	size = _file.readUint32BE();
	byte *f_header = (byte*)malloc(size);
	_file.read(f_header, size);
	do {
		if (READ_BE_UINT32(f_header + pos) == MKID_BE('Bl16')) {
			pos += READ_BE_UINT32(f_header + pos + 4) + 8;
		} else if (READ_BE_UINT32(f_header + pos) == MKID_BE('Wave')) {
			_freq = READ_LE_UINT32(f_header + pos + 8);
			_channels = READ_LE_UINT32(f_header + pos + 12);
			vimaInit(destTable);
			pos += 20;
		} else {
			error("unknown tag");
		}
	} while (pos < size);
	free(f_header);
}

bool Smush::setupAnim(const char *file, int x, int y) {
	if (!_file.open(file))
		return false;

	uint32 tag;
	int32 size;

	tag = _file.readUint32BE();
	assert(tag == MKID_BE('SANM'));
	size = _file.readUint32BE();
	tag = _file.readUint32BE();
	assert(tag == MKID_BE('SHDR'));
	size = _file.readUint32BE();
	byte *s_header = (byte *)malloc(size);
	_file.read(s_header, size);
	_nbframes = READ_LE_UINT32(s_header + 2);
	_x = x;
	_y = y;
	int width = READ_LE_UINT16(s_header + 8);
	int height = READ_LE_UINT16(s_header + 10);

	if ((_width != width) || (_height != height)) {
		_blocky16.init(width, height);
	}

	_width = width;
	_height = height;

	_speed = READ_LE_UINT32(s_header + 14);
	free(s_header);

	return true;
}

bool Smush::play(const char *filename, int x, int y) {
	stop();

	// Load the video
	if (!setupAnim(filename, x, y))
		return false;

	handleFramesHeader();

	SDL_Surface* image;
	image = SDL_CreateRGBSurface(SDL_SWSURFACE, _width, _height, 16, 0x0000f800, 0x000007e0, 0x0000001f, 0x00000000);
	SDL_Surface* buf_image;
	buf_image = SDL_CreateRGBSurface(SDL_SWSURFACE, _width, _height, 16, 0x0000f800, 0x000007e0, 0x0000001f, 0x00000000);

	SDL_Rect src;
	src.x = 0;
	src.y = 0;
	src.w = image->w;
	src.h = image->h;

	_dst = (byte *)image->pixels;
	_buf = (byte *)buf_image->pixels;

	_updateNeeded = false;

	init();

	return true;
}

zlibFile::zlibFile() {
	_handle = NULL;
}

zlibFile::~zlibFile() {
	close();
}

bool zlibFile::open(const char *filename) {
	char flags = 0;
	printf("allocing: ");
	inBuf = (char*)calloc(1, 16385);
	printf("alloced\n");

	if (_handle) {
		warning("File %s already opened", filename);
		return false;
	}

	if (filename == NULL || *filename == 0)
		return false;

	warning("Opening zlibFile %s...", filename);

	_handle = ResourceLoader::instance()->openNewStream(filename);
	if (!_handle) {
		warning("zlibFile %s not found", filename);
		return false;
	}

	// Read in the GZ header
	fread(inBuf, 2, sizeof(char), _handle);				// Header
	fread(inBuf, 1, sizeof(char), _handle);				// Method
	fread(inBuf, 1, sizeof(char), _handle); flags=inBuf[0];		// Flags
	fread(inBuf, 6, sizeof(char), _handle);				// XFlags

	if (((flags & 0x04) != 0) || ((flags & 0x10) != 0))		// Xtra & Comment
		error("Unsupported header flag");

	if ((flags & 0x08) != 0) {					// Orig. Name
		do {
			fread(inBuf, 1, sizeof(char), _handle);
		} while(inBuf[0] != 0);
	}

	if ((flags & 0x02) != 0) // CRC
		fread(inBuf, 2, sizeof(char), _handle);

	memset(inBuf, 0, 16384);			// Zero buffer (debug)
	stream.zalloc = NULL;
	stream.zfree = NULL;
	stream.opaque = Z_NULL;

	if (inflateInit2(&stream, -15) != Z_OK)
		error("inflateInit2 failed");

	stream.next_in = NULL;
	stream.next_out = NULL;
	stream.avail_in = 0;
	stream.avail_out = 16384;

	warning("Opened zlibFile %s...", filename);

	return true;
}

void zlibFile::close() {
	_handle = NULL;
	printf("Closing..\n");
	free(inBuf);
}

bool zlibFile::isOpen() {
	return _handle != NULL;
}

bool zlibFile::eof() {
	error("zlibFile::eof() - Not implemented");
	return false;
}

uint32 zlibFile::pos() {
	error("zlibFile::pos() - Not implemented");
	return false;
}

uint32 zlibFile::size() {
	error("zlibFile::size() - Not implemented");
	return false;
}

void zlibFile::seek(int32 offs, int whence) {
	error("zlibFile::seek() - Not implemented");
}

uint32 zlibFile::read(void *ptr, uint32 len) {
	int result = Z_OK;
	bool fileEOF = false;

	if (_handle == NULL) {
		error("File is not open!");
		return 0;
	}

	if (len == 0)
		return 0;

	stream.next_out = (Bytef*)ptr;
	stream.avail_out = len;

	fileDone = false;
	while (stream.avail_out != 0) {
		if (stream.avail_in == 0) {	// !eof
	        	stream.avail_in = fread(inBuf, 1, 16384, _handle);
			if (stream.avail_in == 0) {
				fileEOF = true;
				break;
			}
			stream.next_in = (Byte*)inBuf;
		}

		result = inflate(&stream, Z_NO_FLUSH);
		if (result == Z_STREAM_END) {	// EOF
			warning("Stream ended");
			fileDone = true;
			break;
		}
		if (result == Z_DATA_ERROR) {
			warning("Decompression error");
			fileDone = true;
			break;
		}
		if (result != Z_OK || fileEOF) {
			warning("Unknown decomp result: %d/%d\n", result, fileEOF);
			fileDone = true;
			break;
		}
	}

	return (int)(len - stream.avail_out);
}
 
byte zlibFile::readByte() {
	unsigned char c;

	read(&c, 1);
	return c;
}

uint16 zlibFile::readUint16LE() {
	uint16 a = readByte();
	uint16 b = readByte();
	return a | (b << 8);
}

uint32 zlibFile::readUint32LE() {
	uint32 a = readUint16LE();
	uint32 b = readUint16LE();
	return (b << 16) | a;
}

uint16 zlibFile::readUint16BE() {
	uint16 b = readByte();
	uint16 a = readByte();
	return a | (b << 8);
}

uint32 zlibFile::readUint32BE() {
	uint32 b = readUint16BE();
	uint32 a = readUint16BE();
	return (b << 16) | a;
}

