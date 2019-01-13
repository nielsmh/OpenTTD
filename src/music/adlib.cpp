/*
* This file is part of OpenTTD.
* OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
* OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
* See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <http://www.gnu.org/licenses/>.
*/

/** @file adlib.cpp Decoding and playback of TTD DOS AdLib music. */

#include "../stdafx.h"
#include "adlib.h"
#include "../mixer.h"
#include "../base_media_base.h"
#include "../core/endian_func.hpp"
#include <vector>

namespace OPL2 {
#include "emu/opl.cpp"
}


/** Decoder for AdLib music data */
struct AdlibPlayer {
	/** Playback status for a pseudo-MIDI track */
	struct TrackStatus {
		byte cur_program;
		byte running_status;
		byte volume;
		byte unk1;
		uint16 delay;
		uint16 unk2;
		uint32 playpos;
		uint32 trackstart;
		uint32 callreturn;
	};
	/** Internal channel status for an OPL2 channel */
	struct ChannelStatus {
		byte unk[11];
	};
	/** Definition of an instrument program */
	struct InstrumentDef {
		byte unk0, unk1, unk2, unk3;
		byte op1_atkdec, op2_atkdec;
		byte op1_susrel, op2_susrel;
		byte op1_wfs, op2_wfs;
		byte ch_syntype;
		byte padding[13];
	};

	TrackStatus tracks[16];
	ChannelStatus channels[9];
	static InstrumentDef base_instruments[17];
	std::vector<InstrumentDef> extra_instruments;
	std::vector<uint32> segments;

	enum class Status {
		STOPPED,
		PLAYING,
		FINISHED,
		BEGIN_PLAY,
	};

	Status status;         ///< current playback status
	byte volume;           ///< current volume level (0-127)
	byte *songdata;        ///< owned copy of raw song data
	size_t songdatalen;    ///< length of songdata buffer
	size_t songtime;       ///< samples rendered
	uint rate;             ///< samplerate of playback

	uint16 song_tempo;

	bool IsPlaying()
	{
		return this->status == Status::PLAYING || this->status == Status::BEGIN_PLAY;
	}

	void ResetDevice()
	{
		// TODO: reset the OPL3 to silence
	}

	void RenderBuffer(int16 *buffer, size_t samples)
	{
		if (!this->IsPlaying()) return;

		OPL2::adlib_getsample(buffer, samples);

		for (size_t i = 0; i < samples; i++) {
			buffer[0] = buffer[0] * this->volume / 127;
			buffer[1] = buffer[1] * this->volume / 127;
			buffer += 2;
		}
	}

	void LoadSong(byte *data, size_t length)
	{
		assert(data != nullptr);
		assert(length > 0);

		this->UnloadSong();

		this->songtime = 0;
		this->songdata = data;
		this->songdatalen = length;
		this->status = Status::BEGIN_PLAY;

		for (auto &tr : this->tracks) {
			tr.trackstart = 0;
		}

		ptrdiff_t pos = 0;
		// first byte has initial tempo
		this->song_tempo = this->songdata[pos++];

		// second byte has number of extra instrument definitions to load
		uint16 num_extra_instruments = this->songdata[pos++];
		this->extra_instruments.clear();
		for (uint16 i = 0; i < num_extra_instruments; i++) {
			InstrumentDef instr;
			MemCpyT(&instr, (InstrumentDef*)(this->songdata + pos), 1);
			this->extra_instruments.push_back(instr);
			pos += sizeof(InstrumentDef);
		}

		// after instrument defs is a count of callable segments and the segments themselves
		uint16 numsegments = this->songdata[pos++];
		this->segments.clear();
		for (uint16 i = 0; i < numsegments; i++) {
			this->segments.push_back(pos + 4);
			pos += FROM_LE16(*(const uint16 *)(this->songdata + pos));
		}

		// after segments follows count of master tracks and the tracks themselves
		uint16 numtracks = this->songdata[pos++];
		for (uint16 i = 0; i < numtracks; i++) {
			byte tr = this->songdata[pos + 4];
			this->tracks[tr].trackstart = pos + 5;
			pos += FROM_LE16(*(const uint16 *)(this->songdata + pos));
		}
	}

	void UnloadSong()
	{
		free(this->songdata);
		this->songdata = nullptr;
		this->segments.clear();
		songdatalen = 0;
		ResetDevice();
		this->status = Status::STOPPED;
	}
};

static AdlibPlayer _adlib;

AdlibPlayer::InstrumentDef AdlibPlayer::base_instruments[] = {
	{ 0x0F, 0x42, 0x3F, 0x3F, 0xFA, 0xFA, 0x41, 0x44, 0x02, 0x03, 0x0F },
	{ 0x0F, 0x02, 0x3F, 0x3F, 0xFA, 0xFA, 0x51, 0x44, 0x02, 0x03, 0x0F },
	{ 0x0F, 0x04, 0x3F, 0x3F, 0xE7, 0xDC, 0x51, 0x46, 0x02, 0x00, 0x0F },
	{ 0x10, 0x00, 0x3E, 0x3F, 0xF8, 0xD5, 0xFF, 0xFF, 0x00, 0x00, 0x09 },
	{ 0x10, 0x01, 0x32, 0x3F, 0xF8, 0xD5, 0x96, 0x86, 0x00, 0x00, 0x0D },
	{ 0x11, 0x10, 0x3F, 0x3F, 0x8F, 0xC8, 0xB4, 0x4A, 0x03, 0x00, 0x0D },
	{ 0x08, 0x0F, 0x3F, 0x3F, 0xF1, 0xF7, 0xFF, 0xFF, 0x00, 0x00, 0x0F },
	{ 0x0F, 0x02, 0x3F, 0x3F, 0xEA, 0xDA, 0x51, 0x46, 0x00, 0x03, 0x0F },
	{ 0x0F, 0x02, 0x3F, 0x3F, 0xEA, 0xDA, 0x51, 0x44, 0x00, 0x03, 0x0F },
	{ 0x02, 0x00, 0x3C, 0x3F, 0xF5, 0xF8, 0x15, 0x47, 0x00, 0x00, 0x0F },
	{ 0x02, 0x01, 0x39, 0x3F, 0xF5, 0xF8, 0x10, 0x46, 0x00, 0x00, 0x0F },
	{ 0x28, 0x2F, 0x3F, 0x3F, 0xFA, 0xF8, 0xF7, 0xF4, 0x00, 0x00, 0x0F },
	{ 0x10, 0x01, 0x32, 0x3F, 0xF8, 0xD5, 0x96, 0x86, 0x00, 0x00, 0x0F },
	{ 0x10, 0x00, 0x3F, 0x3F, 0xE9, 0xD7, 0xD4, 0xC5, 0x03, 0x00, 0x07 },
	{ 0x10, 0x10, 0x32, 0x3F, 0xF8, 0xD7, 0x96, 0x86, 0x00, 0x00, 0x0F },
	{ 0x10, 0x10, 0x32, 0x3F, 0xF8, 0xD4, 0x96, 0x86, 0x00, 0x00, 0x0F },
	{ 0x00, 0x10, 0x32, 0x3F, 0xF8, 0xD4, 0x96, 0x86, 0x02, 0x00, 0x0F },
};


static void RenderMusic(int16 *buffer, size_t samples)
{
	_adlib.RenderBuffer(buffer, samples);
}


const char * MusicDriver_AdLib::Start(const char * const * param)
{
	_adlib.rate = MxSetMusicSource(RenderMusic);
	OPL2::adlib_init(_adlib.rate);

	return nullptr;
}

void MusicDriver_AdLib::Stop()
{
	MxSetMusicSource(nullptr);
}

void MusicDriver_AdLib::PlaySong(const MusicSongInfo & song)
{
	assert(song.filetype == MTT_MPSADLIB);

	_adlib.UnloadSong();

	size_t songlen = 0;
	byte *songdata = GetMusicCatEntryData(song.filename, song.cat_index, songlen);
	_adlib.LoadSong(songdata, songlen);
}

void MusicDriver_AdLib::StopSong()
{
	_adlib.UnloadSong();
}

bool MusicDriver_AdLib::IsSongPlaying()
{
	return _adlib.IsPlaying();
}

void MusicDriver_AdLib::SetVolume(byte vol)
{
	_adlib.volume = vol;
}


MusicDriver *GetAdLibMusicDriver()
{
	static MusicDriver_AdLib driver;
	return &driver;
}
