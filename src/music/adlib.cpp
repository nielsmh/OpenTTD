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
#define OPLTYPE_IS_OPL3
#include "emu/opl.cpp"
}


/** Decoder for AdLib music data */
struct AdlibPlayer {
	/** Playback status for a pseudo-MIDI track */
	struct TrackStatus {
		byte program;
		byte running_status;
		byte volume;
		int8 pitchbend;
		uint16 delay;
		uint16 field12; // only written
		uint32 playpos;
		uint32 trackstart;
		uint32 callreturn;
		byte byte1547; // not originally in the trackstatus struct, but appears to relate to it
	};
	/** Internal channel status for an OPL2 channel */
	struct ChannelStatus {
		byte cur_note;
		byte owning_track;
		byte cur_program;
		uint16 cur_freqnum;
		byte cur_bn_fh; // last register B0+ch value used, with key-on bit clear
		byte velocity;
		uint16 contest; // how many program changes this channel usage has survived without being claimed
		byte padding[2];
	};
	/** Definition of a "note" for percussion channel */
	struct PercussionNote {
		byte b1, b2, b3;
	};
	/** Definition of an instrument program */
	struct InstrumentDef {
		byte op1_tvsk_fmf; // Tremolo, Vibrato, Sustain, KSR, Frequency Multiplication Factor
		byte op2_tvsk_fmf;
		byte op1_ks_vol; // Key Scale, Volume (inverse attenuation)
		byte unk3;
		byte op1_atkdec, op2_atkdec;
		byte op1_susrel, op2_susrel;
		byte op1_wfs, op2_wfs;
		byte ch_syntype;
		byte padding[13];
	};
	/** */
	struct ChannelOperators {
		byte op1;
		byte op2;
	};

	TrackStatus tracks[16];
	ChannelStatus channels[9];
	std::vector<InstrumentDef> extra_instruments;
	std::vector<uint32> segments;
	static uint16 note_freqency[128];
	static byte note_blocknum[128];
	static byte pitchbend_scale[128];
	static PercussionNote perc_notes[32];
	static InstrumentDef perc_instruments[17];
	static ChannelOperators channel_operators[9];

	enum class Status {
		STOPPED,
		PLAYING,
		FINISHED,
		BEGIN_PLAY,
	};

	Status status;              ///< current playback status
	byte volume;                ///< current volume level (0-127)
	byte *songdata;             ///< owned copy of raw song data
	size_t songdatalen;         ///< length of songdata buffer

	/* Keeping track of pcm output */
	uint32 lastsamplewritten;   ///< last sample number written
	double sampletime;          ///< samples emulated + fractional
	double samples_step;        ///< samples per emulation tick
	double steps_sec;           ///< emulation ticks per second

	int16 song_tempo;
	int16 tempo_ticks;
	uint16 active_notes;

	AdlibPlayer()
	{
		this->steps_sec = 60;
		this->status = Status::STOPPED;
		this->songdata = nullptr;
		this->songdatalen = 0;
	}

	bool IsPlaying()
	{
		return this->status == Status::PLAYING || this->status == Status::BEGIN_PLAY;
	}

	uint16 ReadVariableLength(uint32 &pos)
	{
		byte b = 0;
		uint16 res = 0;
		do {
			b = this->songdata[pos++];
			res = (res << 7) + (b & 0x7F);
		} while (b & 0x80);
		return res;
	}

	void ResetDevice()
	{
		/* amusic.com @ 0x07EA = opl_reset */
		for (byte reg = 1; reg <= 0xF5; reg++) {
			OPL2::adlib_write(reg, 0);
		}
		OPL2::adlib_write(0x04, 0x60);
		OPL2::adlib_write(0x04, 0x80);
		OPL2::adlib_write(0x01, 0x20);
		OPL2::adlib_write(0xA8, 0x01);
		OPL2::adlib_write(0x08, 0x40);
		OPL2::adlib_write(0xBD, 0xC0);
	}

	void DoPlayNote(byte tracknum, byte velocity, byte notenum)
	{
		/* amusic.com @ 0x094C = opl_playnote */
		TrackStatus &track = this->tracks[tracknum];

		InstrumentDef *instrument;
		PercussionNote *perc_sound = nullptr;

		if (tracknum == 9) {
			// percussion?
			notenum++;
			byte percnote = notenum - 35;
			if (percnote > 30) return; // out of range
			perc_sound = &perc_notes[percnote];
			instrument = &perc_instruments[perc_sound->b1];
			notenum = perc_sound->b2 - 1;
		} else {
			instrument = &this->extra_instruments.at(track.program);
		}

		if (velocity == 0) {
			for (byte ch = 0; ch < lengthof(this->channels); ch++) {
				ChannelStatus &chst = this->channels[ch];
				if (chst.cur_note == notenum && chst.owning_track == tracknum) {
					chst.cur_note = 0;
					OPL2::adlib_write(0xB0 + ch, chst.cur_bn_fh);
				}
			}
			return;
		}

		bool needprogram = false; // word155F, originally a global used to return extra data from SelectChannel
		byte ch = this->SelectChannel((tracknum == 9) ? (perc_sound->b1 + 128) : track.program, needprogram);
		ChannelStatus &chst = this->channels[ch];

		chst.velocity = velocity;
		chst.cur_note = notenum;
		chst.owning_track = tracknum;

		byte op1 = channel_operators[ch].op1;
		byte op2 = channel_operators[ch].op2;

		if (needprogram) {
			OPL2::adlib_write(0x20 + op1, instrument->op1_tvsk_fmf);
			OPL2::adlib_write(0x20 + op2, instrument->op2_tvsk_fmf);
			int8 keyscale = instrument->op1_ks_vol & 0xC0;
			int8 attenuation = (1 + ~instrument->op1_ks_vol) & 0x3F;
			OPL2::adlib_write(0x40 + op1, keyscale | attenuation);
		}

		OPL2::adlib_write(0xB0 + ch, chst.cur_bn_fh);
		OPL2::adlib_write(0x40 + op2, ((velocity >> 1) ^ 0xFF) & 0x3F);

		if (needprogram) {
			OPL2::adlib_write(0x60 + op1, instrument->op1_atkdec);
			OPL2::adlib_write(0x60 + op2, instrument->op2_atkdec);
			OPL2::adlib_write(0x80 + op1, instrument->op1_susrel);
			OPL2::adlib_write(0x80 + op2, instrument->op2_susrel);
			OPL2::adlib_write(0xE0 + op1, instrument->op1_wfs);
			OPL2::adlib_write(0xE0 + op2, instrument->op2_wfs);
			OPL2::adlib_write(0xC0 + ch, instrument->ch_syntype);
		}

		chst.cur_freqnum = this->CalcFrequency(tracknum, notenum);
		this->DoNoteOn(ch, note_blocknum[notenum], chst.cur_freqnum);
	}

	void DoPitchbend(byte tracknum, byte amount)
	{
		/* amusic.com @ 0x08B5 = opl_pitchbend */
		TrackStatus &track = this->tracks[tracknum];
		track.pitchbend = amount;

		for (uint16 ch = 0; ch < lengthof(this->channels); ch++) {
			ChannelStatus &chst = this->channels[ch];
			if (chst.cur_note == 0) continue;
			if (chst.owning_track != tracknum) continue;
			chst.cur_freqnum = this->CalcFrequency(tracknum, chst.cur_note);
			OPL2::adlib_write(0xA0 + ch, chst.cur_freqnum & 0xFF);
			OPL2::adlib_write(0xB0 + ch, 0x20 | (this->note_blocknum[chst.cur_note] << 2) | (chst.cur_freqnum >> 8));
		}
	}

	bool IsAnyChannelFree()
	{
		/* amusic.com @ 0x0DAF = opl_anychannelfree */
		for (auto &ch : this->channels) {
			if (ch.cur_note == 0) return true;
		}
		return false;
	}

	uint16 CalcFrequency(byte tracknum, uint16 notenum)
	{
		/* amusic.com @ 0x0850 = ? */
		TrackStatus &track = this->tracks[tracknum];

		if (track.pitchbend == 0) {
			return note_freqency[notenum];
		} else if (track.pitchbend > 0) {
			int16 ax = pitchbend_scale[notenum];
			int16 dx = track.pitchbend;
			ax *= dx;
			ax += note_freqency[notenum];
			return ax;
		} else {
			int16 ax = pitchbend_scale[notenum - 1];
			int16 dx = -track.pitchbend;
			ax *= dx;
			ax += note_freqency[notenum];
			return ax;
		}
	}

	byte SelectChannel(byte program, bool &needprogram)
	{
		/* amusic.com @ 0x0BBE = ? */
		for (auto &chst : this->channels) {
			chst.contest++;
		}

		/* amusic.com @ 0x0BD8 = opl_makechannel */
		needprogram = false;

		uint16 maxcontest = 0;
		byte bestch = 0;

		for (byte ch = 0; ch < lengthof(this->channels); ch++) {
			ChannelStatus &chst = this->channels[ch];
			if (chst.contest > maxcontest) {
				maxcontest = chst.contest;
				bestch = ch;
			}
			if (chst.cur_note == 0) {
				needprogram = program != chst.cur_program;
				chst.cur_program = program;
				chst.contest = 0;
				return ch;
			}
		}
		bestch = min<byte>(bestch, 8); // should not be necessary

		ChannelStatus &chst = this->channels[bestch];
		//needprogram = program != chst.cur_program; // not present in original
		chst.cur_program = program;
		chst.contest = 0;
		return bestch;
	}

	void DoNoteOn(byte ch, byte b, uint16 freqnum)
	{
		OPL2::adlib_write(0xA0 + ch, freqnum & 0xFF);
		this->channels[ch].cur_bn_fh = (b << 2) | (freqnum >> 8);
		OPL2::adlib_write(0xb0 + ch, this->channels[ch].cur_bn_fh | 0x20);
	}

	void PlayTrackStep(byte tracknum)
	{
		TrackStatus &track = this->tracks[tracknum];

		/* amusic.com @ 0x0DD7 = track_playstep */
		while (track.delay == 0) {
			byte b1 = this->songdata[track.playpos++];
			byte b2;
			if (b1 == 0xFE) {
				/* segment call */
				b1 = this->songdata[track.playpos++];
				track.callreturn = track.playpos;
				track.playpos = this->segments[b1];
				track.delay = this->ReadVariableLength(track.playpos);
				continue;
			} else if (b1 == 0xFD) {
				/* segment return */
				track.playpos = track.callreturn;
				track.callreturn = 0;
				track.delay = this->ReadVariableLength(track.playpos);
				continue;
			} else if (b1 == 0xFF) {
				/* end song */
				this->status = Status::FINISHED;
				return;
			} else if (b1 >= 0x80) {
				/* new MIDI-ish status byte */
				track.running_status = b1;
				b1 = this->songdata[track.playpos++];
			}

			switch (track.running_status & 0xF0) {
				case 0x80:
					// note off
					b2 = this->songdata[track.playpos++];
					this->DoPlayNote(tracknum, 0, b1);
					if (track.byte1547 != 0) {
						// dual channel play?
						this->DoPlayNote(track.byte1547, 0, b1);
					}
					break;
				case 0x90:
					// note on-off
					b2 = this->songdata[track.playpos++];
					if (b2 != 0) {
						if (track.byte1547 != 0 || !this->IsAnyChannelFree()) {
							// dual channel play?
							TrackStatus &othertrack = this->tracks[track.byte1547];
							othertrack.program = track.program;
							othertrack.pitchbend = track.pitchbend;
							this->DoPlayNote(track.byte1547, b2 * track.volume / 128, b1);
						}
						this->DoPlayNote(tracknum, b2 * track.volume / 128, b1);
						this->active_notes++;
					} else {
						if (this->active_notes != 0) this->active_notes--;
						this->DoPlayNote(track.byte1547, 0, b1);
						this->DoPlayNote(tracknum, 0, b1);
					}
					break;
				case 0xB0:
					// controller?
					b2 = this->songdata[track.playpos++];
					if (b1 == 7) {
						if (b2 != 0) b2++;
						track.volume = b2;
					} else if (b1 == 0) {
						if (b2 != 0) {
							this->song_tempo = (uint32)b2 * 48 / 60;
						}
					} else if (b1 == 0x7E) {
						track.byte1547 = b2 - 1;
					} else if (b1 == 0x7F) {
						track.byte1547 = 0;
					}
					break;
				case 0xC0:
					// program change
					if (b1 == 0x7E) {
						// repeat mark translates to end of song
						this->status = Status::FINISHED;
						return;
					} else {
						track.program = b1;
					}
					break;
				case 0xE0:
					// pitch bend
					track.pitchbend = b1 - 16;
					this->DoPitchbend(tracknum, track.pitchbend);
					if (track.byte1547 != 0) {
						TrackStatus &othertrack = this->tracks[track.byte1547];
						othertrack.pitchbend = track.pitchbend;
						this->DoPitchbend(track.byte1547, track.pitchbend);
					}
					break;
				default:
					break;
			}

			track.delay = this->ReadVariableLength(track.playpos);
		}
	}

	bool PlayStep()
	{
		/* amusic.com @ 0x0CEF = i66f3_playstep */
		if (this->status != Status::PLAYING) return false;

		this->sampletime += this->samples_step;

		this->tempo_ticks -= this->song_tempo;
		if (this->tempo_ticks > 0) return true;
		this->tempo_ticks += 0x94;

		const byte trackorder[16] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 10, 11, 12, 13, 14, 15, 9 };
		for (byte tr : trackorder) {
			TrackStatus &trst = this->tracks[tr];
			if (trst.playpos == 0) continue;
			if (trst.delay == 0) {
				this->PlayTrackStep(tr);
			} else {
				trst.delay--;
			}
		}

		for (auto &ch : this->channels) {
			ch.cur_program = 0xFF;
			ch.cur_note = 0;
		}

		return true;
	}

	void RenderBuffer(int16 *buffer, size_t samples)
	{
		if (!this->IsPlaying()) return;

		if (this->status == Status::BEGIN_PLAY) this->RestartSong();

		uint32 targetsamplewritten = this->lastsamplewritten + (uint32)samples;
		uint32 bufpos = 0;
		while (this->lastsamplewritten < targetsamplewritten) {
			uint32 towrite = min<uint32>((uint32)this->sampletime - this->lastsamplewritten, (uint32)samples - bufpos);
			if (towrite > 0) OPL2::adlib_getsample(buffer + bufpos, towrite);
			this->lastsamplewritten += towrite;
			bufpos += towrite;
			if (bufpos == samples) break; // exhausted pcm buffer, do not play more steps
			if (!PlayStep()) break; // play step, break if end of song
		}

		for (size_t i = 0; i < samples; i++) {
			buffer[0] = buffer[0] * this->volume / 127;
			buffer[1] = buffer[1] * this->volume / 127;
			buffer += 2;
		}
	}

	void RestartSong()
	{
		this->lastsamplewritten = 0;
		this->sampletime = 0;

		/* amusic.com @ 0x126E = restart_song */
		for (auto &tr : this->tracks) {
			tr.pitchbend = 0;
			tr.field12 = 0;
			tr.byte1547 = 0;
			if (tr.trackstart != 0) {
				tr.playpos = tr.trackstart;
				tr.delay = this->ReadVariableLength(tr.playpos);
			} else {
				tr.playpos = 0;
				tr.delay = 0;
			}
		}
		this->status = Status::PLAYING;
	}

	void LoadSong(byte *data, size_t length)
	{
		assert(data != nullptr);
		assert(length > 0);

		this->UnloadSong();

		this->sampletime = 0;
		this->songdata = data;
		this->songdatalen = length;

		/* amusic.com @ 0x1181 = load_song_data */
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

		this->status = Status::BEGIN_PLAY; // this ought to be synchronized
	}

	void UnloadSong()
	{
		free(this->songdata);
		this->songdata = nullptr;
		this->segments.clear();
		this->songdatalen = 0;
		this->active_notes = 0;
		ResetDevice();
		this->status = Status::STOPPED;
	}
};

static AdlibPlayer _adlib;

uint16 AdlibPlayer::note_freqency[] = {
	0x00B5, 0x00C0, 0x00CC, 0x00D8, 0x00E5, 0x00F2, 0x0101, 0x0110,
	0x0120, 0x0131, 0x0143, 0x0157, 0x016B, 0x0181, 0x0198, 0x01B0,
	0x01CA, 0x01E5, 0x0202, 0x0220, 0x0241, 0x0263, 0x0287, 0x02AE,
	0x016B, 0x0181, 0x0198, 0x01B0, 0x01CA, 0x01E5, 0x0202, 0x0220,
	0x0241, 0x0263, 0x0287, 0x02AE, 0x016B, 0x0181, 0x0198, 0x01B0,
	0x01CA, 0x01E5, 0x0202, 0x0220, 0x0241, 0x0263, 0x0287, 0x02AE,
	0x016B, 0x0181, 0x0198, 0x01B0, 0x01CA, 0x01E5, 0x0202, 0x0220,
	0x0241, 0x0263, 0x0287, 0x02AE, 0x016B, 0x0181, 0x0198, 0x01B0,
	0x01CA, 0x01E5, 0x0202, 0x0220, 0x0241, 0x0263, 0x0287, 0x02AE,
	0x016B, 0x0181, 0x0198, 0x01B0, 0x01CA, 0x01E5, 0x0202, 0x0220,
	0x0241, 0x0263, 0x0287, 0x02AE, 0x016B, 0x0181, 0x0198, 0x01B0,
	0x01CA, 0x01E5, 0x0202, 0x0220, 0x0241, 0x0263, 0x0287, 0x02AE,
	0x016B, 0x0181, 0x0198, 0x01B0, 0x01CA, 0x01E5, 0x0202, 0x0220,
	0x0241, 0x0263, 0x0287, 0x02AE // 108 values defined, allocated for 128 to avoid overrun risks from bad data
};
byte AdlibPlayer::note_blocknum[] = {
	0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0,
	1, 1, 1, 1, 1, 1, 1, 1,
	1, 1, 1, 1, 2, 2, 2, 2,
	2, 2, 2, 2, 2, 2, 2, 2,
	3, 3, 3, 3, 3, 3, 3, 3,
	3, 3, 3, 3, 4, 4, 4, 4,
	4, 4, 4, 4, 4, 4, 4, 4,
	5, 5, 5, 5, 5, 5, 5, 5,
	5, 5, 5, 5, 6, 6, 6, 6,
	6, 6, 6, 6, 6, 6, 6, 6,
	7, 7, 7, 7, 7, 7, 7, 7,
	7, 7, 7, 7, 8, 8, 8, 8,
	8, 8, 8, 8, 8, 8, 8, 8,
};
byte AdlibPlayer::pitchbend_scale[] = {
	3, 3, 3, 3, 4, 4, 4, 4,
	4, 5, 5, 5, 3, 3, 3, 3,
	4, 4, 4, 4, 4, 5, 5, 5,
	3, 3, 3, 3, 4, 4, 4, 4,
	4, 5, 5, 5, 3, 3, 3, 3,
	4, 4, 4, 4, 4, 5, 5, 5,
	3, 3, 3, 3, 4, 4, 4, 4,
	4, 5, 5, 5, 3, 3, 3, 3,
	4, 4, 4, 4, 4, 5, 5, 5,
	3, 3, 3, 3, 4, 4, 4, 4,
	4, 5, 5, 5, 3, 3, 3, 3,
	4, 4, 4, 4, 4, 5, 5, 5,
	3, 3, 3, 3, 4, 4, 4, 4,
	4, 5, 5, 5
};
AdlibPlayer::PercussionNote AdlibPlayer::perc_notes[] = {
	{ 0x03, 0x15, 0x64 },
	{ 0x03, 0x17, 0x64 },
	{ 0x05, 0x31, 0x64 },
	{ 0x0A, 0x1C, 0x55 },
	{ 0x06, 0x28, 0x4D },
	{ 0x09, 0x18, 0x55 },
	{ 0x04, 0x1C, 0x64 },
	{ 0x07, 0x52, 0x4D },
	{ 0x04, 0x1F, 0x64 },
	{ 0x07, 0x52, 0x4D },
	{ 0x0C, 0x21, 0x64 },
	{ 0x08, 0x52, 0x4D },
	{ 0x0C, 0x25, 0x64 },
	{ 0x0C, 0x28, 0x64 },
	{ 0x00, 0x3E, 0x64 },
	{ 0x0C, 0x2C, 0x50 },
	{ 0x01, 0x3E, 0x4D },
	{ 0x00, 0x3E, 0x64 },
	{ 0x01, 0x3F, 0x4D },
	{ 0x02, 0x3E, 0x4D },
	{ 0x00, 0x41, 0x64 },
	{ 0x0B, 0x0C, 0x4D },
	{ 0x00, 0x3E, 0x64 },
	{ 0xFF, 0xFF, 0xFF }, // this one looks dangerous, idx=23
	{ 0x01, 0x3F, 0x4D },
	{ 0x0D, 0x43, 0x55 },
	{ 0x0D, 0x3D, 0x55 },
	{ 0x0E, 0x3E, 0x64 },
	{ 0x0F, 0x31, 0x64 },
	{ 0x0F, 0x2C, 0x55 },
	{ 0x10, 0x36, 0x4D },
	{ 0x10, 0x31, 0x4D },
};
AdlibPlayer::InstrumentDef AdlibPlayer::perc_instruments[] = {
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

AdlibPlayer::ChannelOperators AdlibPlayer::channel_operators[] = {
	{ 0x00, 0x03 }, { 0x01, 0x04 }, { 0x02, 0x05 },
	{ 0x08, 0x0B }, { 0x09, 0x0C }, { 0x0A, 0x0D },
	{ 0x10, 0x13 }, { 0x11, 0x14 }, { 0x12, 0x15 },
};


static void RenderMusic(int16 *buffer, size_t samples)
{
	_adlib.RenderBuffer(buffer, samples);
}


const char * MusicDriver_AdLib::Start(const char * const * param)
{
	uint32 rate = MxSetMusicSource(RenderMusic);
	_adlib.samples_step = rate / _adlib.steps_sec;
	OPL2::adlib_init(rate);

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
	static bool started = false;
	if (!started) {
		driver.Start(nullptr);
		started = true;
	}
	return &driver;
}
