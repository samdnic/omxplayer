/*
 * 
 *      Copyright (C) 2012 Edgar Hucek
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include <stdio.h>
#include <signal.h>
#include <stdlib.h>
#include <stdint.h>
#include <termios.h>
#include <sys/mman.h>
#include <linux/fb.h>
#include <sys/ioctl.h>
#include <getopt.h>
#include <string.h>

#define AV_NOWARN_DEPRECATED

extern "C"
{
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
}
;

#include "OMXStreamInfo.h"

#include "utils/log.h"

#include "DllAvUtil.h"
#include "DllAvFormat.h"
#include "DllAvFilter.h"
#include "DllAvCodec.h"
#include "linux/RBP.h"

#include "OMXVideo.h"
#include "OMXAudioCodecOMX.h"
#include "utils/PCMRemap.h"
#include "OMXClock.h"
#include "OMXAudio.h"
#include "OMXReader.h"
#include "OMXPlayerVideo.h"
#include "OMXPlayerAudio.h"
#include "OMXPlayerSubtitles.h"
#include "DllOMX.h"
#include "Srt.h"

#include <string>
#include <utility>

#include "PiVTNetwork.h"
#include "PiVTConfig.h"

typedef enum
{
	CONF_FLAGS_FORMAT_NONE, CONF_FLAGS_FORMAT_SBS, CONF_FLAGS_FORMAT_TB
} FORMAT_3D_T;

CRBP g_RBP;
COMXCore g_OMX;
TV_DISPLAY_STATE_T tv_state;
enum PCMChannels *m_pChannelMap = NULL;
volatile sig_atomic_t g_abort = false;
bool m_bMpeg = false;
bool m_passthrough = false;
bool m_Deinterlace = false;
bool m_HWDecode = false;
std::string deviceString = "omx:local";
int m_use_hw_audio = false;
std::string m_external_subtitles_path;
bool m_has_external_subtitles = false;
std::string m_font_path = "/usr/share/fonts/truetype/freefont/FreeSans.ttf";
bool m_has_font = false;
float m_font_size = 0.055f;
bool m_centered = false;
unsigned int m_subtitle_lines = 3;
bool m_Pause = false;
OMXReader *m_omx_reader = NULL;
OMXReader *m_omx_reader_next = NULL;
bool m_omx_reader_openok;
pthread_t m_omx_reader_thread;
bool m_dump_format = false;
int m_audio_index_use = -1;
int m_seek_pos = 0;
bool m_buffer_empty = true;
bool m_thread_player = false;
OMXClock *m_av_clock = NULL;
COMXStreamInfo m_hints_audio;
COMXStreamInfo m_hints_video;
OMXPacket *m_omx_pkt = NULL;
bool m_hdmi_clock_sync = false;
bool m_no_hdmi_clock_sync = false;
bool m_stop = false;
int m_subtitle_index = -1;
DllBcmHost m_BcmHost;
OMXPlayerVideo m_player_video;
OMXPlayerAudio m_player_audio;
OMXPlayerSubtitles m_player_subtitles;
int m_tv_show_info = 0;
bool m_has_video = false;
bool m_has_audio = false;
bool m_has_subtitle = false;
float m_display_aspect = 0.0f;
bool m_boost_on_downmix = false;
bool m_loop = false;
double startpts = 0;

std::string threadfile = "";

CRect DestRect = { 0, 0, 0, 0 };

enum
{
	ERROR = -1, SUCCESS, ONEBYTE
};

static struct termios orig_termios;
static void restore_termios()
{
	tcsetattr(STDIN_FILENO, TCSANOW, &orig_termios);
}

static int orig_fl;
static void restore_fl()
{
	fcntl(STDIN_FILENO, F_SETFL, orig_fl);
}

void sig_handler(int s)
{
	printf("strg-c catched\n");
	signal(SIGINT, SIG_DFL);
	g_abort = true;
}

void print_usage()
{
	printf("Usage: omxplayer [OPTIONS] [FILE...]\n");
	printf("Options :\n");
	printf("         -h / --help                    print this help\n");
//  printf("         -a / --alang language          audio language        : e.g. ger\n");
	printf("         -n / --aidx  index             audio stream index    : e.g. 1\n");
	printf("         -o / --adev  device            audio out device      : e.g. hdmi/local\n");
	printf("         -i / --info                    dump stream format and exit\n");
	printf("         -s / --stats                   pts and buffer stats\n");
	printf("         -p / --passthrough             audio passthrough\n");
	printf("         -d / --deinterlace             deinterlacing\n");
	printf("         -w / --hw                      hw audio decoding\n");
	printf("         -3 / --3d mode                 switch tv into 3d mode (e.g. SBS/TB)\n");
	printf("         -y / --hdmiclocksync           adjust display refresh rate to match video (default)\n");
	printf("         -z / --nohdmiclocksync         do not adjust display refresh rate to match video\n");
	printf("         -t / --sid index               show subtitle with index\n");
	printf("         -r / --refresh                 adjust framerate/resolution to video\n");
	printf("         -l / --pos                     start position (in seconds)\n");
	printf("         -L / --loop                    loop files endlessly\n");
	printf("              --boost-on-downmix        boost volume when downmixing\n");
	printf("              --subtitles path          external subtitles in UTF-8 srt format\n");
	printf("              --font path               subtitle font\n");
	printf("                                        (default: /usr/share/fonts/truetype/freefont/FreeSans.ttf)\n");
	printf("              --font-size size          font size as thousandths of screen height\n");
	printf("                                        (default: 55)\n");
	printf("              --align left/center       subtitle alignment (default: left)\n");
	printf("              --lines n                 number of lines to accommodate in the subtitle buffer\n");
	printf("                                        (default: 3)\n");
	printf("              --win \"x1 y1 x2 y2\"       Set position of video window\n");
}

void PrintSubtitleInfo()
{
	auto count = m_omx_reader->SubtitleStreamCount();
	size_t index = 0;

	if (m_has_external_subtitles)
	{
		++count;
		if (!m_player_subtitles.GetUseExternalSubtitles())
			index = m_player_subtitles.GetActiveStream() + 1;
	}
	else if (m_has_subtitle)
	{
		index = m_player_subtitles.GetActiveStream();
	}

	printf("Subtitle count: %d, state: %s, index: %d, delay: %d\n", count,
			m_has_subtitle && m_player_subtitles.GetVisible() ? " on" : "off", index + 1,
			m_has_subtitle ? m_player_subtitles.GetDelay() : 0);
}

bool Exists(const std::string& path)
{
	struct stat buf;
	auto error = stat(path.c_str(), &buf);
	return !error || errno != ENOENT;
}

bool IsURL(const std::string& str)
{
	auto result = str.find("://");
	if (result == std::string::npos || result == 0)
		return false;

	for (size_t i = 0; i < result; ++i)
	{
		if (!isalpha(str[i]))
			return false;
	}
	return true;
}

void PrintFileNotFound(const std::string& path)
{
	printf("File \"%s\" not found.\n", path.c_str());
}

void *
reader_open_thread(void *data)
{
	std::string *filename = (std::string *) data;

	if (!IsURL(*filename) && !Exists(*filename))
	{
		PrintFileNotFound(*filename);
		m_omx_reader_openok = 0;
		return 0;
	}

	if (m_has_external_subtitles && !Exists(m_external_subtitles_path))
	{
		PrintFileNotFound(m_external_subtitles_path);
		m_omx_reader_openok = 0;
		return 0;
	}

	m_omx_reader_next = new OMXReader;
	printf("thread nextreader %p (%d)\n", m_omx_reader_next, m_omx_reader_next->AudioStreamCount());
	m_omx_reader_openok = m_omx_reader_next->Open(*filename, m_dump_format);
	printf("thread opened %p (%d) openok %d\n", m_omx_reader_next, m_omx_reader_next->AudioStreamCount(), m_omx_reader_openok);
	return m_omx_reader_next;
}

void start_omx()
{
	g_RBP.Initialize();
	g_OMX.Initialize();

	m_av_clock = new OMXClock();
}

void stop_omx()
{
	vc_tv_show_info(0);

	delete m_av_clock;

	g_OMX.Deinitialize();
	g_RBP.Deinitialize();
}

void stop_player(bool m_refresh)
{
	if (m_has_video && m_refresh && tv_state.display.hdmi.group && tv_state.display.hdmi.mode)
	{
		m_BcmHost.vc_tv_hdmi_power_on_explicit_new(HDMI_MODE_HDMI, (HDMI_RES_GROUP_T) tv_state.display.hdmi.group, tv_state.display.hdmi.mode);
	}

	m_av_clock->OMXStop();
	m_av_clock->OMXStateIdle();

	m_player_subtitles.Close();
	m_player_video.Close();
	m_player_audio.Close();
}

void SetSpeed(int iSpeed)
{
	if (!m_av_clock)
		return;

	if (iSpeed < OMX_PLAYSPEED_PAUSE)
		return;

	m_omx_reader->SetSpeed(iSpeed);

	if (m_av_clock->OMXPlaySpeed() != OMX_PLAYSPEED_PAUSE && iSpeed == OMX_PLAYSPEED_PAUSE)
		m_Pause = true;
	else if (m_av_clock->OMXPlaySpeed() == OMX_PLAYSPEED_PAUSE && iSpeed != OMX_PLAYSPEED_PAUSE)
		m_Pause = false;

	m_av_clock->OMXSpeed(iSpeed);
}

static float get_display_aspect_ratio(HDMI_ASPECT_T aspect)
{
	float display_aspect;
	switch (aspect)
	{
		case HDMI_ASPECT_4_3:
			display_aspect = 4.0 / 3.0;
			break;
		case HDMI_ASPECT_14_9:
			display_aspect = 14.0 / 9.0;
			break;
		case HDMI_ASPECT_16_9:
			display_aspect = 16.0 / 9.0;
			break;
		case HDMI_ASPECT_5_4:
			display_aspect = 5.0 / 4.0;
			break;
		case HDMI_ASPECT_16_10:
			display_aspect = 16.0 / 10.0;
			break;
		case HDMI_ASPECT_15_9:
			display_aspect = 15.0 / 9.0;
			break;
		case HDMI_ASPECT_64_27:
			display_aspect = 64.0 / 27.0;
			break;
		default:
			display_aspect = 16.0 / 9.0;
			break;
	}
	return display_aspect;
}

static float get_display_aspect_ratio(SDTV_ASPECT_T aspect)
{
	float display_aspect;
	switch (aspect)
	{
		case SDTV_ASPECT_4_3:
			display_aspect = 4.0 / 3.0;
			break;
		case SDTV_ASPECT_14_9:
			display_aspect = 14.0 / 9.0;
			break;
		case SDTV_ASPECT_16_9:
			display_aspect = 16.0 / 9.0;
			break;
		default:
			display_aspect = 4.0 / 3.0;
			break;
	}
	return display_aspect;
}

void FlushStreams(double pts)
{
//  if(m_av_clock)
//    m_av_clock->OMXPause();

	if (m_has_video)
		m_player_video.Flush();

	if (m_has_audio)
		m_player_audio.Flush();

	if (m_has_subtitle)
		m_player_subtitles.Flush(pts);

	if (m_omx_pkt)
	{
		m_omx_reader->FreePacket(m_omx_pkt);
		m_omx_pkt = NULL;
	}

	if (pts != DVD_NOPTS_VALUE)
		m_av_clock->OMXUpdateClock(pts);

//  if(m_av_clock)
//  {
//    m_av_clock->OMXReset();
//    m_av_clock->OMXResume();
//  }
}

void SetVideoMode(int width, int height, int fpsrate, int fpsscale, FORMAT_3D_T is3d)
{
	int32_t num_modes = 0;
	int i;
	HDMI_RES_GROUP_T prefer_group;
	HDMI_RES_GROUP_T group = HDMI_RES_GROUP_CEA;
	float fps = 60.0f; // better to force to higher rate if no information is known
	uint32_t prefer_mode;

	if (fpsrate && fpsscale)
		fps = DVD_TIME_BASE / OMXReader::NormalizeFrameduration((double) DVD_TIME_BASE * fpsscale / fpsrate);

	//Supported HDMI CEA/DMT resolutions, preferred resolution will be returned
	TV_SUPPORTED_MODE_NEW_T *supported_modes = NULL;
	// query the number of modes first
	int max_supported_modes = m_BcmHost.vc_tv_hdmi_get_supported_modes_new(group, NULL, 0, &prefer_group, &prefer_mode);

	if (max_supported_modes > 0)
		supported_modes = new TV_SUPPORTED_MODE_NEW_T[max_supported_modes];

	if (supported_modes)
	{
		num_modes = m_BcmHost.vc_tv_hdmi_get_supported_modes_new(group, supported_modes, max_supported_modes, &prefer_group, &prefer_mode);

		CLog::Log(LOGDEBUG, "EGL get supported modes (%d) = %d, prefer_group=%x, prefer_mode=%x\n", group, num_modes, prefer_group, prefer_mode);
	}

	TV_SUPPORTED_MODE_NEW_T *tv_found = NULL;

	if (num_modes > 0 && prefer_group != HDMI_RES_GROUP_INVALID)
	{
		uint32_t best_score = 1 << 30;
		uint32_t scan_mode = 0;

		for (i = 0; i < num_modes; i++)
		{
			TV_SUPPORTED_MODE_NEW_T *tv = supported_modes + i;
			uint32_t score = 0;
			uint32_t w = tv->width;
			uint32_t h = tv->height;
			uint32_t r = tv->frame_rate;

			/* Check if frame rate match (equal or exact multiple) */
			if (fabs(r - 1.0f * fps) / fps < 0.002f)
				score += 0;
			else if (fabs(r - 2.0f * fps) / fps < 0.002f)
				score += 1 << 8;
			else
				score += (1 << 28) / r; // bad - but prefer higher framerate

			/* Check size too, only choose, bigger resolutions */
			if (width && height)
			{
				/* cost of too small a resolution is high */
				score += max((int) (width - w), 0) * (1 << 16);
				score += max((int) (height - h), 0) * (1 << 16);
				/* cost of too high a resolution is lower */
				score += max((int) (w - width), 0) * (1 << 4);
				score += max((int) (h - height), 0) * (1 << 4);
			}

			// native is good
			if (!tv->native)
				score += 1 << 16;

			// interlace is bad
			if (scan_mode != tv->scan_mode)
				score += (1 << 16);

			// wanting 3D but not getting it is a negative
			if (is3d == CONF_FLAGS_FORMAT_SBS && !(tv->struct_3d_mask & HDMI_3D_STRUCT_SIDE_BY_SIDE_HALF_HORIZONTAL))
				score += 1 << 18;
			if (is3d == CONF_FLAGS_FORMAT_TB && !(tv->struct_3d_mask & HDMI_3D_STRUCT_TOP_AND_BOTTOM))
				score += 1 << 18;

			// prefer square pixels modes
			float par = get_display_aspect_ratio((HDMI_ASPECT_T) tv->aspect_ratio) * (float) tv->height / (float) tv->width;
			score += fabs(par - 1.0f) * (1 << 12);

			/*printf("mode %dx%d@%d %s%s:%x par=%.2f score=%d\n", tv->width, tv->height,
			 tv->frame_rate, tv->native?"N":"", tv->scan_mode?"I":"", tv->code, par, score);*/

			if (score < best_score)
			{
				tv_found = tv;
				best_score = score;
			}
		}
	}

	if (tv_found)
	{
		printf("Output mode %d: %dx%d@%d %s%s:%x\n", tv_found->code, tv_found->width, tv_found->height, tv_found->frame_rate,
				tv_found->native ? "N" : "", tv_found->scan_mode ? "I" : "", tv_found->code);
		// if we are closer to ntsc version of framerate, let gpu know
		int ifps = (int) (fps + 0.5f);
		bool ntsc_freq = fabs(fps * 1001.0f / 1000.0f - ifps) < fabs(fps - ifps);
		char response[80];
		vc_gencmd(response, sizeof response, "hdmi_ntsc_freqs %d", ntsc_freq);

		/* inform TV of any 3D settings. Note this property just applies to next hdmi mode change, so no need to call for 2D modes */
		HDMI_PROPERTY_PARAM_T property;
		property.property = HDMI_PROPERTY_3D_STRUCTURE;
		property.param1 = HDMI_3D_FORMAT_NONE;
		property.param2 = 0;
		if (is3d != CONF_FLAGS_FORMAT_NONE)
		{
			if (is3d == CONF_FLAGS_FORMAT_SBS && tv_found->struct_3d_mask & HDMI_3D_STRUCT_SIDE_BY_SIDE_HALF_HORIZONTAL)
				property.param1 = HDMI_3D_FORMAT_SBS_HALF;
			else if (is3d == CONF_FLAGS_FORMAT_TB && tv_found->struct_3d_mask & HDMI_3D_STRUCT_TOP_AND_BOTTOM)
				property.param1 = HDMI_3D_FORMAT_TB_HALF;
			m_BcmHost.vc_tv_hdmi_set_property(&property);
		}

		printf("ntsc_freq:%d %s%s\n", ntsc_freq,
				property.param1 == HDMI_3D_FORMAT_SBS_HALF ? "3DSBS" : "",
				property.param1 == HDMI_3D_FORMAT_TB_HALF ? "3DTB" : "");
		m_BcmHost.vc_tv_hdmi_power_on_explicit_new(HDMI_MODE_HDMI, (HDMI_RES_GROUP_T) group, tv_found->code);
	}
}

void run_loaded_video()
{
	FlushStreams(startpts);
	if (m_has_video) m_player_video.UnFlush();

	m_thread_player = true;

	if (!m_omx_reader_openok)
	{
		printf("reader not openok. emergency exit!!!\n");
		m_stop = 1;
	}

	// Try and detect if stream has changed
	COMXStreamInfo m_hints_video_next;

	//m_omx_reader->GetHints(OMXSTREAM_AUDIO, m_hints_audio);
	m_omx_reader_next->GetHints(OMXSTREAM_VIDEO, m_hints_video_next);

	if (m_hints_video_next.height != m_hints_video.height ||
			m_hints_video_next.width != m_hints_video.width ||
			m_hints_video_next.fpsrate != m_hints_video.fpsrate ||
			m_hints_video_next.fpsscale != m_hints_video.fpsscale)
	{
		m_hints_video = m_hints_video_next;
		SetVideoMode(m_hints_video.width, m_hints_video.height, m_hints_video.fpsrate, m_hints_video.fpsscale, CONF_FLAGS_FORMAT_NONE);
		m_player_video.Close();
		m_player_video.Open(m_hints_video, m_av_clock, DestRect, m_Deinterlace,  m_bMpeg,
							   m_hdmi_clock_sync, m_thread_player, m_display_aspect);
	}

	if (m_omx_reader)
	{
		m_omx_reader->Close();
		delete m_omx_reader;
		m_player_audio.SetReader(m_omx_reader_next);
	}

	m_omx_reader = m_omx_reader_next;

	m_av_clock->OMXStop();
	m_av_clock->OMXStateIdle();
	m_player_video.Reset();

	if (m_audio_index_use != -1)
		m_omx_reader->SetActiveStream(OMXSTREAM_AUDIO, m_audio_index_use);

	m_av_clock->SetSpeed(DVD_PLAYSPEED_NORMAL);
	m_av_clock->OMXStateExecute();
	m_av_clock->OMXStart(0.0);

}

void load_background_video(std::string filepath)
{
	threadfile = filepath;
	pthread_create(&m_omx_reader_thread, NULL, reader_open_thread, &threadfile);
}

int main(int argc, char *argv[])
{
	PiVT_Config config;
	PiVT_Network network(config.get_port());

	std::string nextvideo = config.get_stopvideo();
	std::string currentvideo = config.get_stopvideo();

	signal(SIGINT, sig_handler);

	if (isatty(STDIN_FILENO))
	{
		struct termios new_termios;

		tcgetattr(STDIN_FILENO, &orig_termios);

		new_termios = orig_termios;
		new_termios.c_lflag &= ~(ICANON | ECHO | ECHOCTL | ECHONL);
		new_termios.c_cflag |= HUPCL;
		new_termios.c_cc[VMIN] = 0;

		tcsetattr(STDIN_FILENO, TCSANOW, &new_termios);
		atexit(restore_termios);
	}
	else
	{
		orig_fl = fcntl(STDIN_FILENO, F_GETFL);
		fcntl(STDIN_FILENO, F_SETFL, orig_fl | O_NONBLOCK);
		atexit(restore_fl);
	}

	std::string m_filename;
	double m_incr = 0;
	bool m_stats = false;
	FORMAT_3D_T m_3d = CONF_FLAGS_FORMAT_NONE;
	bool m_refresh = false;

	const int font_opt = 0x100;
	const int font_size_opt = 0x101;
	const int align_opt = 0x102;
	const int subtitles_opt = 0x103;
	const int lines_opt = 0x104;
	const int pos_opt = 0x105;
	const int boost_on_downmix_opt = 0x200;

	struct option longopts[] = { { "info", no_argument, NULL, 'i' }, { "help", no_argument, NULL, 'h' }, { "aidx", required_argument, NULL, 'n' }, { "adev", required_argument, NULL, 'o' }, { "stats", no_argument, NULL, 's' }, { "passthrough", no_argument, NULL, 'p' }, { "deinterlace", no_argument, NULL, 'd' }, { "hw", no_argument, NULL, 'w' }, { "3d", required_argument, NULL, '3' }, { "hdmiclocksync", no_argument, NULL, 'y' }, { "nohdmiclocksync", no_argument, NULL, 'z' }, { "refresh", no_argument, NULL, 'r' }, { "sid", required_argument, NULL, 't' }, { "pos", required_argument, NULL, 'l' }, { "loop", no_argument, NULL, 'L' }, { "font", required_argument, NULL, font_opt }, { "font-size", required_argument, NULL, font_size_opt }, { "align", required_argument, NULL, align_opt }, { "subtitles", required_argument, NULL, subtitles_opt }, { "lines", required_argument, NULL, lines_opt }, { "win", required_argument, NULL, pos_opt }, { "boost-on-downmix", no_argument, NULL, boost_on_downmix_opt }, { 0, 0, 0, 0 } };

	int c;
	std::string mode;
	while ((c = getopt_long(argc, argv, "wihn:l:o:cslpd3:yzt:rL", longopts, NULL)) != -1)
	{
		switch (c)
		{
			case 'r':
				m_refresh = true;
				break;
			case 'y':
				m_hdmi_clock_sync = true;
				break;
			case 'z':
				m_no_hdmi_clock_sync = true;
				break;
			case '3':
				mode = optarg;
				if (mode != "SBS" && mode != "TB")
				{
					print_usage();
					return 0;
				}
				if (mode == "TB")
					m_3d = CONF_FLAGS_FORMAT_TB;
				else
					m_3d = CONF_FLAGS_FORMAT_SBS;
				break;
			case 'd':
				m_Deinterlace = true;
				break;
			case 'w':
				m_use_hw_audio = true;
				break;
			case 'p':
				m_passthrough = true;
				break;
			case 's':
				m_stats = true;
				break;
			case 'o':
				deviceString = optarg;
				if (deviceString != "local" && deviceString != "hdmi")
				{
					print_usage();
					return 0;
				}
				deviceString = "omx:" + deviceString;
				break;
			case 'i':
				m_dump_format = true;
				break;
			case 't':
				m_subtitle_index = atoi(optarg) - 1;
				if (m_subtitle_index < 0)
					m_subtitle_index = 0;
				break;
			case 'n':
				m_audio_index_use = atoi(optarg) - 1;
				if (m_audio_index_use < 0)
					m_audio_index_use = 0;
				break;
			case 'l':
				m_seek_pos = atoi(optarg);
				if (m_seek_pos < 0)
					m_seek_pos = 0;
				break;
			case font_opt:
				m_font_path = optarg;
				m_has_font = true;
				break;
			case font_size_opt:
			{
				const int thousands = atoi(optarg);
				if (thousands > 0)
					m_font_size = thousands * 0.001f;
			}
				break;
			case align_opt:
				m_centered = !strcmp(optarg, "center");
				break;
			case subtitles_opt:
				m_external_subtitles_path = optarg;
				m_has_external_subtitles = true;
				break;
			case lines_opt:
				m_subtitle_lines = std::max(atoi(optarg), 1);
				break;
			case pos_opt:
				sscanf(optarg, "%f %f %f %f", &DestRect.x1, &DestRect.y1, &DestRect.x2, &DestRect.y2);
				break;
			case boost_on_downmix_opt:
				m_boost_on_downmix = true;
				break;
			case 'L':
				m_loop = true;
				break;
			case 0:
				break;
			case 'h':
				print_usage();
				return 0;
				break;
			case ':':
				return 0;
				break;
			default:
				return 0;
				break;
		}
	}

	if (optind >= argc)
	{
		print_usage();
		return 0;
	}

	if (m_has_font && !Exists(m_font_path))
	{
		PrintFileNotFound(m_font_path);
		return 0;
	}

	CLog::Init("./");

	start_omx();

	m_filename = std::string(config.get_stopvideo());

	if (!Exists(m_filename))
	{
		printf(std::string("Stop video " + m_filename + " not found. Exiting.\n").c_str());
		goto do_exit;
	}

	reader_open_thread(&m_filename);

	m_thread_player = true;

	if (!m_omx_reader_openok)
	{
		printf("reader not openok. emergency exit!!!\n");
		m_stop = 1;
	}

	if (m_omx_reader)
	{
		m_omx_reader->Close();
		delete m_omx_reader;
		m_player_audio.SetReader(m_omx_reader_next);
	}

	m_omx_reader = m_omx_reader_next;

	m_bMpeg = m_omx_reader->IsMpegVideo();
	m_has_video = m_omx_reader->VideoStreamCount();
	m_has_audio = m_omx_reader->AudioStreamCount();
	m_has_subtitle = m_has_external_subtitles || m_omx_reader->SubtitleStreamCount();

	if (m_filename.find("3DSBS") != string::npos || m_filename.find("HSBS") != string::npos)
		m_3d = CONF_FLAGS_FORMAT_SBS;
	else if (m_filename.find("3DTAB") != string::npos || m_filename.find("HTAB") != string::npos)
		m_3d = CONF_FLAGS_FORMAT_TB;

	// 3d modes don't work without switch hdmi mode
	if (m_3d != CONF_FLAGS_FORMAT_NONE)
		m_refresh = true;

	// you really don't want want to match refresh rate without hdmi clock sync
	if (m_refresh && !m_no_hdmi_clock_sync)
		m_hdmi_clock_sync = true;

	if (!m_av_clock->OMXInitialize(m_has_video, m_has_audio))
	{
		printf("avclock error. emergency exit!!!\n");
		m_stop = 1;
		goto do_exit;
	}

	if (m_hdmi_clock_sync && !m_av_clock->HDMIClockSync())
	{
		printf("hdmi clock sync error. emergency exit!!!\n");
		m_stop = 1;
		goto do_exit;
	}

	m_omx_reader->GetHints(OMXSTREAM_AUDIO, m_hints_audio);
	m_omx_reader->GetHints(OMXSTREAM_VIDEO, m_hints_video);

	if (m_has_video && m_refresh)
	{
		memset(&tv_state, 0, sizeof(TV_DISPLAY_STATE_T));
		m_BcmHost.vc_tv_get_display_state(&tv_state);

		SetVideoMode(m_hints_video.width, m_hints_video.height, m_hints_video.fpsrate, m_hints_video.fpsscale, m_3d);
	}

	// get display aspect
	TV_DISPLAY_STATE_T current_tv_state;
	memset(&current_tv_state, 0, sizeof(TV_DISPLAY_STATE_T));
	m_BcmHost.vc_tv_get_display_state(&current_tv_state);
	if (current_tv_state.state & (VC_HDMI_HDMI | VC_HDMI_DVI))
	{
		//HDMI or DVI on
		m_display_aspect = get_display_aspect_ratio((HDMI_ASPECT_T)(current_tv_state.display.hdmi.aspect_ratio));
	}
	else
	{
		//composite on
		m_display_aspect = get_display_aspect_ratio((SDTV_ASPECT_T)(current_tv_state.display.sdtv.display_options.aspect));
	}
	m_display_aspect *= (float) (current_tv_state.display.hdmi.height) / (float) (current_tv_state.display.hdmi.width);
	if (m_has_video && !m_player_video.Open(m_hints_video, m_av_clock, DestRect, m_Deinterlace, m_bMpeg, m_hdmi_clock_sync, m_thread_player, m_display_aspect))
	{
		printf("video open error. emergency exit!!!\n");
		m_stop = 1;
		goto do_exit;
	}
	while (m_has_audio && !m_player_audio.Open(m_hints_audio, m_av_clock, m_omx_reader, deviceString, m_passthrough, m_use_hw_audio, m_boost_on_downmix, m_thread_player))
	{
		printf("audio open error. press enter to reset state\n");
		sleep(1);
		while (getchar() == EOF)
			;
		goto do_exit;
	}
	{
		std::vector < Subtitle > external_subtitles;
		if (m_has_external_subtitles && !ReadSrt(m_external_subtitles_path, external_subtitles))
		{
			puts("Unable to read the subtitle file.");
			goto do_exit;
		}
		if (m_has_subtitle && !m_player_subtitles.Open(m_omx_reader->SubtitleStreamCount(), std::move(external_subtitles), m_font_path, m_font_size, m_centered, m_subtitle_lines, m_av_clock))
		{
			printf("subtitles open error. emergency exit!!!\n");
			m_stop = 1;
			goto do_exit;
		}
	}
	if (m_has_subtitle)
	{
		if (!m_has_external_subtitles)
		{
			if (m_subtitle_index != -1)
			{
				m_player_subtitles.SetActiveStream(std::min(m_subtitle_index, m_omx_reader->SubtitleStreamCount() - 1));
			}
			m_player_subtitles.SetUseExternalSubtitles(false);
		}

		if (m_subtitle_index == -1 && !m_has_external_subtitles)
			m_player_subtitles.SetVisible(false);
	}

	m_thread_player = true;


	if (m_audio_index_use != -1)
		m_omx_reader->SetActiveStream(OMXSTREAM_AUDIO, m_audio_index_use);

	m_av_clock->SetSpeed(DVD_PLAYSPEED_NORMAL);
	m_av_clock->OMXStateExecute();
	m_av_clock->OMXStart(0.0);

	struct timespec starttime, endtime;

	// Load the stop video ready for next time
	load_background_video(config.get_stopvideo());

	// m_av_clock->OMXReset();
	while (!m_stop)
	{
		if (g_abort)
			goto do_exit;

		// Process network commands
		PiVT_CommandData netcommand = network.tick();
		switch (netcommand.command)
		{
			case PIVT_PLAY:
			{
				if (Exists(config.get_videosfolder() + netcommand.arg))
				{
					// Load file if needed
					if (netcommand.arg != nextvideo)
					{
						nextvideo = netcommand.arg;
						pthread_join(m_omx_reader_thread, NULL);
						std::string fullpath = std::string(config.get_videosfolder() + nextvideo);
						threadfile = fullpath;
						reader_open_thread(&threadfile);
					}

					m_filename = std::string(config.get_videosfolder() + nextvideo);
					currentvideo = nextvideo;
					nextvideo = config.get_stopvideo();

					// Play the file
					pthread_join(m_omx_reader_thread, NULL);
					run_loaded_video();
					load_background_video(config.get_stopvideo());

					// Report some info
					std::stringstream ss;
					int length = m_omx_reader->GetStreamLength() / 1000.0f;
					ss << "Playing " << netcommand.arg << " " << length << " seconds long";
					netcommand.conn->writeData(ss.str());
				}
				else
				{
					netcommand.conn->writeData("404 File: " + netcommand.arg + " not found!");
				}
				break;
			}
			case PIVT_LOAD:
			{
				if (Exists(config.get_videosfolder() + netcommand.arg))
				{
					nextvideo = netcommand.arg;
					pthread_join(m_omx_reader_thread, NULL);
					load_background_video(std::string(config.get_videosfolder() + nextvideo));
				}
				else
				{
					netcommand.conn->writeData("404 File: " + config.get_videosfolder() + netcommand.arg + " not found!");
				}
				break;
			}
			case PIVT_STOP:
			{
				// Load file if needed
				if (nextvideo != config.get_stopvideo())
				{
					nextvideo = config.get_stopvideo();
					pthread_join(m_omx_reader_thread, NULL);
					std::string fullpath = std::string(config.get_videosfolder() + nextvideo);
					threadfile = fullpath;
					reader_open_thread(&threadfile);
				}

				m_filename = std::string(config.get_videosfolder() + nextvideo);
				currentvideo = nextvideo;
				nextvideo = config.get_stopvideo();

				// Play the file
				run_loaded_video();
				load_background_video(config.get_stopvideo());
				break;
			}
			case PIVT_INFO:
			{
				std::stringstream ss;
				ss << "200 ";

				if (m_filename.compare(config.get_stopvideo()))
				{
					ss << "Playing " << currentvideo << ", ";
				}
				else
				{
					ss << "Stopped, ";
				}

				if (config.get_stopvideo().compare(nextvideo))
				{
					ss << "Loaded " << nextvideo << ", ";
				}
				else
				{
					ss << "No video loaded, ";
				}

				double pts = m_av_clock->GetPTS();
			    int remain = int(float((m_omx_reader->GetStreamLength()/1000.0f) - (pts / DVD_TIME_BASE)));

			    ss << remain << " seconds remain";

			    netcommand.conn->writeData(ss.str());

			    break;
			}
			case PIVT_QUIT:
			    printf("Lost a client.\r\n");
			    break;
			default:
			{
				// Does nothing, accounts for no command
				break;
			}
		}

		if (m_Pause)
		{
			OMXClock::OMXSleep(2);
			continue;
		}

		if (m_incr != 0 && !m_bMpeg)
		{
			int seek_flags = 0;
			double seek_pos = 0;
			double pts = 0;

			if (m_has_subtitle)
				m_player_subtitles.Pause();

			m_av_clock->OMXStop();

			pts = m_av_clock->GetPTS();

			seek_pos = (pts / DVD_TIME_BASE) + m_incr;
			seek_flags = m_incr < 0.0f ? AVSEEK_FLAG_BACKWARD : 0;

			seek_pos *= 1000.0f;

			m_incr = 0;

			if (m_omx_reader->SeekTime(seek_pos, seek_flags, &startpts))
				FlushStreams(startpts);

			m_player_video.Close();
			if (m_has_video && !m_player_video.Open(m_hints_video, m_av_clock, DestRect, m_Deinterlace, m_bMpeg, m_hdmi_clock_sync, m_thread_player, m_display_aspect))
			{
				printf("video open error. emergency exit!!!\n");
				m_stop = 1;
				goto do_exit;
			}

			m_av_clock->OMXStart(startpts);

			if (m_has_subtitle)
				m_player_subtitles.Resume();
		}

		/* player got in an error state */
		if (m_player_audio.Error())
		{
			printf("audio player error. emergency exit!!!\n");
			m_stop = 1;
			goto do_exit;
		}

		if (m_stats)
		{
#if 0
			CLog::Log(LOGDEBUG, "V : %8.02f %8d %8d A : %8.02f %8.02f Cv : %8d Ca : %8d                            \n",
					m_av_clock->OMXMediaTime(), m_player_video.GetDecoderBufferSize(),
					m_player_video.GetDecoderFreeSpace(), m_player_audio.GetCurrentPTS() / DVD_TIME_BASE,
					m_player_audio.GetDelay(), m_player_video.GetCached(), m_player_audio.GetCached());
#endif
		}

		if (m_omx_reader->IsEof() && !m_omx_pkt)
		{
			if (!m_player_audio.GetCached() && !m_player_video.GetCached())
			{
				run_loaded_video();
				load_background_video(config.get_stopvideo());
			}
			else
			{
				// CLog::Log(LOGDEBUG, "waiting before eof: %d %d\n", m_player_audio.GetCached(), m_player_video.GetCached());
				// Abort audio buffering, now we're on our own
				if (m_buffer_empty)
					m_av_clock->OMXResume();

				OMXClock::OMXSleep(10);
				continue;
			}
		}

		/* when the audio buffer runs under 0.1 seconds we buffer up */
		if (m_has_audio)
		{
			if (m_player_audio.GetDelay() < 0.1f && !m_buffer_empty)
			{
				if (!m_av_clock->OMXIsPaused())
				{
					m_av_clock->OMXPause();
					//printf("buffering start\n");
					m_buffer_empty = true;
					clock_gettime(CLOCK_REALTIME, &starttime);
				}
			}
			if (m_player_audio.GetDelay() > (AUDIO_BUFFER_SECONDS * 0.75f) && m_buffer_empty)
			{
				if (m_av_clock->OMXIsPaused())
				{
					m_av_clock->OMXResume();
					//printf("buffering end\n");
					m_buffer_empty = false;
				}
			}
			if (m_buffer_empty)
			{
				clock_gettime(CLOCK_REALTIME, &endtime);
				if ((endtime.tv_sec - starttime.tv_sec) > 1)
				{
					m_buffer_empty = false;
					m_av_clock->OMXResume();
					//printf("buffering timed out\n");
				}
			}
		}

		if (!m_omx_pkt)
			m_omx_pkt = m_omx_reader->Read();

		if (m_has_video && m_omx_pkt && m_omx_reader->IsActive(OMXSTREAM_VIDEO, m_omx_pkt->stream_index))
		{
			if (m_player_video.AddPacket(m_omx_pkt))
			{
				// CLog::Log(LOGDEBUG, "+packet nok\n");
				m_omx_pkt = NULL;
			}
			else
			{
				// CLog::Log(LOGDEBUG, "+packet ok\n");
				OMXClock::OMXSleep(10);
			}

			if (m_tv_show_info)
			{
				char response[80];
				vc_gencmd(response, sizeof response, "render_bar 4 video_fifo %d %d %d %d", m_player_video.GetDecoderBufferSize() - m_player_video.GetDecoderFreeSpace(), 0, 0, m_player_video.GetDecoderBufferSize());
				vc_gencmd(response, sizeof response, "render_bar 5 audio_fifo %d %d %d %d", (int) (100.0 * m_player_audio.GetDelay()), 0, 0, 100 * AUDIO_BUFFER_SECONDS);
			}
		}
		else if (m_has_audio && m_omx_pkt && m_omx_pkt->codec_type == AVMEDIA_TYPE_AUDIO)
		{
			if (m_player_audio.AddPacket(m_omx_pkt))
				m_omx_pkt = NULL;
			else
				OMXClock::OMXSleep(10);
		}
		else if (m_has_subtitle && m_omx_pkt && m_omx_pkt->codec_type == AVMEDIA_TYPE_SUBTITLE)
		{
			auto result = m_player_subtitles.AddPacket(m_omx_pkt, m_omx_reader->GetRelativeIndex(m_omx_pkt->stream_index));
			if (result)
				m_omx_pkt = NULL;
			else
				OMXClock::OMXSleep(10);
		}
		else
		{
			if (m_omx_pkt)
			{
				m_omx_reader->FreePacket(m_omx_pkt);
				m_omx_pkt = NULL;
			}
		}
	}
	do_exit: printf("\n");
	if (!m_stop && !g_abort)
	{
		if (m_has_audio)
			m_player_audio.WaitCompletion();
		else if (m_has_video)
			m_player_video.WaitCompletion();
	}
	if (m_omx_pkt)
	{
		m_omx_reader->FreePacket(m_omx_pkt);
		m_omx_pkt = NULL;
	}


	stop_player(m_refresh);

	stop_omx();

	printf("have a nice day ;)\n");
	return 1;
}
