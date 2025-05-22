/*

   em-admin.c

   Read and write Engelmann/Lorenz/Brummerhoop watermeter
   radio parameters and read various info via infrared M-Bus interface.

   This project is in no way affiliated with the above-mentioned vendors.

   Tested with water meters from the “M-ETH” “DWZ” series,
   other models may work. Use at your own risk.

   If you set the readout interval too low and also do not limit the hours and days,
   the battery will discharge before the end of the water meter's service life.
   Frequent reading via infrared likewise drains the battery.

   Hardware: An UART adapter with 3mm IR-LED and sensor (2€ in total) is
   sufficient, e.g. adapt https://github.com/openv/openv/wiki/ESPHome-Optolink#hardware

   M-Bus timing and protocol parsing has been loosely implemented
   according to the specification and is “works for me” ware.

   (C) 2025 Hajo Noerenberg

   http://www.noerenberg.de/
   https://github.com/hn/em-admin

   This program is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License version 3.0 as
   published by the Free Software Foundation.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License along
   with this program. If not, see <http://www.gnu.org/licenses/gpl-3.0.txt>.

*/

#include <stdio.h>
#include <stdint.h>
#include <unistd.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>
#include <time.h>
#include <fcntl.h>
#include <syslog.h>
#include <termios.h>
#include <sys/select.h>

/* Do NOT change: */

#define EM_ENA_RADIO_AVAIL	(1 << 0)	/* radio can be turned on */
#define EM_ENA_RADIO_ON		(1 << 1)	/* radio is on */
#define EM_ENA_AES		(1 << 2)	/* enable AES encryption */
#define EM_ENA_STARTVOL		(1 << 3)	/* enable radio if EM_SET_ONVOL liters were used */
#define EM_ENA_STARTDATE	(1 << 4)	/* enable radio after EM_SET_ONDAY date */

#define EM_MONTH_JAN		(1 << 0)
#define EM_MONTH_FEB		(1 << 1)
#define EM_MONTH_MAR		(1 << 2)
#define EM_MONTH_APR		(1 << 3)
#define EM_MONTH_MAY		(1 << 4)
#define EM_MONTH_JUN		(1 << 5)
#define EM_MONTH_JUL		(1 << 6)
#define EM_MONTH_AUG		(1 << 7)
#define EM_MONTH_SEP		(1 << 8)
#define EM_MONTH_OCT		(1 << 9)
#define EM_MONTH_NOV		(1 << 10)
#define EM_MONTH_DEC		(1 << 11)

#define EM_WEEKOM_1		0b00000000000000000000000011111111	/* days 1-8 */
#define EM_WEEKOM_2		0b00000000000000000111111100000000	/* days 9-15 */
#define EM_WEEKOM_3		0b00000000011111111000000000000000	/* days 16-23 */
#define EM_WEEKOM_4		0b01111111100000000000000000000000	/* days 24-31 */

#define EM_DAYOW_MON		(1 << 0)
#define EM_DAYOW_TUE		(1 << 1)
#define EM_DAYOW_WED		(1 << 2)
#define EM_DAYOW_THU		(1 << 3)
#define EM_DAYOW_FRI		(1 << 4)
#define EM_DAYOW_SAT		(1 << 5)
#define EM_DAYOW_SUN		(1 << 6)

#define EM_HOUR(h)		(1 << (h))

#define EM_OMSMODE_T1_OMS3_ENC5	1
#define EM_OMSMODE_C1_OMS3_ENC5	3
#define EM_OMSMODE_T1_OMS4_ENC7	17
#define EM_OMSMODE_C1_OMS4_ENC7	19

#define EM_FRAME_SHORT		17
#define EM_FRAME_LONG		18

#define MBUS_WAKEUP_TIME	3
#define MBUS_WAKEUP_CHAR	0x55
#define MBUS_FRAME_ACK		0xE5
#define MBUS_FRAME_SHORT_START	0x10
#define MBUS_FRAME_LONG_START	0x68
#define MBUS_FRAME_STOP		0x16
#define MBUS_C_SND_UD		0x53
#define MBUS_C_REQ_UD2		0x7b
#define MBUS_CI_DATA_SEND	0x51
#define MBUS_CI_RSPUD12		0x72
#define MBUS_FRAME_SHORT_HDR_LEN (1 + 1 + 1)			/* START C A */
#define MBUS_FRAME_LONG_HDR_LEN	(4 + 1 + 1 + 1)			/* START C A CI */
#define MBUS_FRAME_FTR_LEN	(1 + 1)				/* CHK STOP */
#define MBUS_RSPUD12_HDR_LEN	(4 + 2 + 1 + 1 + 1 + 1 + 2)	/* AD MAN VER MED ACC STAT SIG */

#define LOG_BUFSIZE		256

/* Do change according to your needs: */

#define EM_SET_FLAGS		( EM_ENA_RADIO_AVAIL | EM_ENA_RADIO_ON | EM_ENA_AES )

#define EM_SET_INTERVAL		7 * 60		/* seconds */

#define EM_SET_MONTHS		( EM_MONTH_JAN | EM_MONTH_FEB | EM_MONTH_MAR | EM_MONTH_APR | \
				EM_MONTH_MAY | EM_MONTH_JUN | EM_MONTH_JUL | EM_MONTH_AUG | \
				EM_MONTH_SEP | EM_MONTH_OCT | EM_MONTH_NOV | EM_MONTH_DEC )

#define EM_SET_WEEKOMS		( EM_WEEKOM_1 | EM_WEEKOM_2 | EM_WEEKOM_3 | EM_WEEKOM_4 )

#define EM_SET_DAYOWS		( EM_DAYOW_MON | EM_DAYOW_TUE | EM_DAYOW_WED | EM_DAYOW_THU | \
				EM_DAYOW_FRI | EM_DAYOW_SAT | EM_DAYOW_SUN )

#define EM_SET_HOURS		( EM_HOUR(0) | EM_HOUR(1) | EM_HOUR(2) | EM_HOUR(3) | EM_HOUR(4) | \
				EM_HOUR(5) | EM_HOUR(6) | EM_HOUR(7) | EM_HOUR(8) | EM_HOUR(9) | \
				EM_HOUR(10) | EM_HOUR(11) | EM_HOUR(12) | EM_HOUR(13) | EM_HOUR(14) | \
				EM_HOUR(15) | EM_HOUR(16) | EM_HOUR(17) | EM_HOUR(18) | EM_HOUR(19) | \
				EM_HOUR(20) | EM_HOUR(21) | EM_HOUR(22) | EM_HOUR(23) )

#define EM_SET_ONDAY_YEAR	2024		/* only used if EM_ENA_STARTDATE is set */
#define EM_SET_ONDAY_MONTH	1
#define EM_SET_ONDAY_DAY	1

#define EM_SET_ONVOL		1000		/* only used if EM_ENA_STARTVOL is set */

#define EM_SET_OPYEARS		10
#define EM_SET_OMSMODE		EM_OMSMODE_C1_OMS3_ENC5
#define EM_SET_FRAMETYPE	EM_FRAME_LONG

#define EM_SET_KEYDAY_MONTH	10
#define EM_SET_KEYDAY_DAY	3


void log_line(int prio, const char *fmt, ...) {
	va_list args;
	va_start(args, fmt);
	if (prio <= LOG_DEBUG) {
		vfprintf(stdout, fmt, args);
		fprintf(stdout, "\n");
	}
	va_end(args);
}

unsigned char *bitprint(unsigned char *data, const unsigned long val, const unsigned int len) {
	for (unsigned int i = 0; i < len; i++)
		data[i] = (val & (1 << (len - i - 1))) ? '1' : '0';
	data[len] = 0;
	return data;
}

int serial_interface_attribs(int fd, int speed, int parity) {
	struct termios tty;

	if (tcgetattr(fd, &tty) < 0)
		return -1;

	cfsetospeed(&tty, (speed_t) speed);
	cfsetispeed(&tty, (speed_t) speed);

	tty.c_cflag |= (CLOCAL | CREAD);	/* ignore modem controls */
	tty.c_cflag &= ~CSIZE;
	tty.c_cflag |= CS8;			/* 8-bit characters */
	tty.c_cflag &= ~PARENB;			/* no parity bit */
	tty.c_cflag |= parity;			/* (not) set parity bit */
	tty.c_cflag &= ~CSTOPB;			/* only need 1 stop bit */
	tty.c_cflag &= ~CRTSCTS;		/* no hardware flowcontrol */

	/* setup for non-canonical mode */
	tty.c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL | IXON);
	tty.c_lflag &= ~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
	tty.c_oflag &= ~OPOST;

	/* fetch bytes as they become available */
	tty.c_cc[VMIN] = 1;
	tty.c_cc[VTIME] = 1;

	if (tcsetattr(fd, TCSANOW, &tty) != 0)
		return -1;

	return 0;
}

ssize_t serial_write(int fd, const unsigned char *data, const size_t len) {
	char buf[LOG_BUFSIZE];
	for (unsigned int i = 0; (i < len) && (i < (LOG_BUFSIZE / 3 - 1)); i++) {
		sprintf(buf + (3 * i), "%02x ", data[i]);
	}
	log_line(LOG_DEBUG, "UART>%03d> %s", len, buf);
	ssize_t n = write(fd, data, len);
	return n;
}

ssize_t serial_read(int fd, unsigned char *data, const size_t maxbytes) {
	char buf[LOG_BUFSIZE];
	fd_set serial_read_fds;
	struct timeval serial_timeout;
	size_t p = 0;

	FD_ZERO(&serial_read_fds);
	FD_SET(fd, &serial_read_fds);
	serial_timeout.tv_sec = 1;
	serial_timeout.tv_usec = 0;

	while (p < maxbytes) {
		if (select(fd + 1, &serial_read_fds, NULL, NULL, &serial_timeout) == 1) {
			p += read(fd, data + p, maxbytes - p);
		} else {
			break;
		}
	}

	if (!p) {
		log_line(LOG_DEBUG, "UART< (read timeout)");
		return -EIO;
	}

	for (size_t i = 0; i < p; i++) {
		if ((3 * (i + 1) + 32) >= LOG_BUFSIZE) {
			sprintf(buf + (3 * i), "(%ld bytes not shown)", p - i);
			break;
		}
		sprintf(buf + (3 * i), "%02x ", data[i]);
	}
	log_line(LOG_DEBUG, "UART<%03d< %s", p, buf);
	return p;
}

unsigned char mbus_cslong(const unsigned char *frame, const size_t len) {
	unsigned char cs = 0;
	for (unsigned int i = 4; i < (len - 2); i++)
		cs += frame[i];
	return cs;
}

int mbus_checklong(const unsigned char *data, const size_t len) {
	unsigned char ll;
	if (len < (MBUS_FRAME_LONG_HDR_LEN + MBUS_FRAME_FTR_LEN)) {
		log_line(LOG_ERR, "M-Bus long frame: Too small");
		return 0;
	}
	if ((data[0] != MBUS_FRAME_LONG_START) || (data[3] != MBUS_FRAME_LONG_START)) {
		log_line(LOG_ERR, "M-Bus long frame: Invalid start header");
		return 0;
	}
	if (data[1] != data[2]) {
		log_line(LOG_ERR, "M-Bus long frame: Mismatching length info");
		return 0;
	}
	ll = data[1] + 4 + MBUS_FRAME_FTR_LEN;
	if (ll > len) {
		log_line(LOG_ERR, "M-Bus long frame: Frame length %d exceeds buffer size %d", ll, len);
		return 0;
	}
	if (data[ll - 1] != MBUS_FRAME_STOP) {
		log_line(LOG_ERR, "M-Bus long frame: Invalid stop header");
		return 0;
	}
	if (mbus_cslong(data, ll) != data[ll - MBUS_FRAME_FTR_LEN]) {
		log_line(LOG_ERR, "M-Bus long frame: Invalid checksum");
		return 0;
	}
	/* EN1434-3 Dedicated Application Layer, Chapter 3 */
	log_line(LOG_INFO, "MBUS_C: 0x%02x", data[4]);
	log_line(LOG_INFO, "MBUS_ADR: %d", data[5]);
	log_line(LOG_INFO, "MBUS_CI: 0x%02x", data[6]);
	if (data[6] == MBUS_CI_RSPUD12) {
		log_line(LOG_INFO, "MBUS_SECADR: 0x%02x%02x%02x%02x", data[10], data[9], data[8], data[7]);
		log_line(LOG_INFO, "MBUS_MANUFACTURER: 0x%02X%02X (%c%c%c)", data[12], data[11],
			 64 + ((data[12] & 0b1111100) >> 2),
			 64 + ((((data[12] << 8) | data[11]) & 0b1111100000) >> 5),
			 64 + (data[11] & 0b11111));					/* 0x12FA = DWZ = Lorenz GmbH */
		log_line(LOG_INFO, "MBUS_VERSION: %d", data[13]);
		log_line(LOG_INFO, "MBUS_MEDIUM: 0x%02x", data[14]);			/* 0x07 = Water */
		log_line(LOG_INFO, "MBUS_ACCESSCOUNT: %d", data[15]);
		log_line(LOG_INFO, "MBUS_STATE: 0x%02x", data[16]);
		log_line(LOG_INFO, "MBUS_SIGNATURE: 0x%02X%02X", data[18], data[17]);
	}
	return ll;
}

int mbus_checkshort(const unsigned char *data, const size_t len) {
	if (len < (MBUS_FRAME_SHORT_HDR_LEN + MBUS_FRAME_FTR_LEN)) {
		log_line(LOG_ERR, "M-Bus short frame: Too small");
		return 0;
	}
	if (data[0] != MBUS_FRAME_SHORT_START) {
		log_line(LOG_ERR, "M-Bus short frame: Invalid start header");
		return 0;
	}
	if (data[MBUS_FRAME_SHORT_HDR_LEN + MBUS_FRAME_FTR_LEN - 1] != MBUS_FRAME_STOP) {
		log_line(LOG_ERR, "M-Bus short frame: Invalid stop header");
		return 0;
	}
	if ((unsigned char)(data[1] + data[2]) != data[MBUS_FRAME_SHORT_HDR_LEN]) {
		log_line(LOG_ERR, "M-Bus short frame: Invalid checksum");
		return 0;
	}

	log_line(LOG_INFO, "MBUS_C: 0x%02x", data[1]);
	log_line(LOG_INFO, "MBUS_ADR: %d", data[2]);
	return MBUS_FRAME_SHORT_HDR_LEN + MBUS_FRAME_FTR_LEN;
}

ssize_t mbus_io(int fd, unsigned char *out, const size_t outlen, unsigned char *in, size_t inlen) {
	if ((outlen == (MBUS_FRAME_SHORT_HDR_LEN + MBUS_FRAME_FTR_LEN)) && (out[0] == MBUS_FRAME_SHORT_START)) {
		out[outlen - MBUS_FRAME_FTR_LEN] = out[1] + out[2];
		if (!mbus_checkshort(out, outlen)) return -EPROTO;
	} else if ((outlen >= (MBUS_FRAME_LONG_HDR_LEN + MBUS_FRAME_FTR_LEN)) && (out[0] == MBUS_FRAME_LONG_START)) {
		out[outlen - MBUS_FRAME_FTR_LEN] = mbus_cslong(out, outlen);
		if (!mbus_checklong(out, outlen)) return -EPROTO;
	} else {
		return -EPROTO;
	}
	serial_write(fd, out, outlen);
	return serial_read(fd, in, inlen);
}

ssize_t mbus_io_acked(int fd, unsigned char *out, const size_t outlen) {
	unsigned char in[8];
	ssize_t len = mbus_io(fd, out, outlen, in, sizeof(in));
	if (len < 0) {
		log_line(LOG_ERR, "M-Bus i/o failed: %s", strerror(-len));
		return -len;
	}

	if ((len < 1) || (in[0] != MBUS_FRAME_ACK)) {
		log_line(LOG_ERR, "M-Bus protocol error, received %d unprocessable bytes.", len);
		return EPROTO;
	}

	log_line(LOG_INFO, "Operation completed successfully");
	return 0;
}

void mbus_wakeup(int fd) {
	unsigned char frame_out[25];
	memset(frame_out, MBUS_WAKEUP_CHAR, sizeof(frame_out));
	log_line(LOG_INFO, "Sending wakeup bytes");
	for (unsigned int i = 0; i < 20; i++)
		serial_write(fd, frame_out, sizeof(frame_out));
}

int em_set_time(int fd) {
	time_t rawtime;
	struct tm *timeinfo;
	time(&rawtime);
	timeinfo = localtime(&rawtime);
	/* Device time is supposed to be standard (no DST) time */
	if (timeinfo->tm_isdst > 0) {
		rawtime -= 60 * 60;		/* this is might be wrong somewhere on the planet */
		timeinfo = localtime(&rawtime);
	}

	log_line(LOG_INFO,
		 "Setting device time to: %02d.%02d.%d %02d:%02d (no DST)",
		 timeinfo->tm_mday, timeinfo->tm_mon + 1,
		 timeinfo->tm_year + 1900, timeinfo->tm_hour, timeinfo->tm_min);

	unsigned char frame_out[] = {
		MBUS_FRAME_LONG_START,
		10,				/* Frame length */
		10,				/* Frame length */
		MBUS_FRAME_LONG_START,
		MBUS_C_SND_UD,			/* Control */
		254,				/* Address */
		MBUS_CI_DATA_SEND,		/* Control info */
		0x04, 0xed, 0x00,		/* DIF, VIF, VIFE: set time */
		timeinfo->tm_min,		/* 4 bytes CP32 "F" type time and date */
		timeinfo->tm_hour,
		((((timeinfo->tm_year + 1900 - 2000) & 0b00000111) << 5) | timeinfo->tm_mday),
		((((timeinfo->tm_year + 1900 - 2000) & 0b01111000) << 1) | (timeinfo->tm_mon + 1)),
		0x00,				/* Checksum */
		MBUS_FRAME_STOP
	};

	return mbus_io_acked(fd, frame_out, sizeof(frame_out));
}

int em_set_keyday(int fd) {
	unsigned char frame_out[] = {
		MBUS_FRAME_LONG_START,
		8,				/* Frame length */
		8,				/* Frame length */
		MBUS_FRAME_LONG_START,
		MBUS_C_SND_UD,			/* Control */
		254,				/* Address */
		MBUS_CI_DATA_SEND,		/* Control info */
		0x02, 0xec,			/* DIF, VIF: set key date */
		0x00,				/* Unknown or reserved */
		(EM_SET_KEYDAY_DAY | 0b11100000),
		(EM_SET_KEYDAY_MONTH | 0b11110000),
		0x00,				/* Checksum */
		MBUS_FRAME_STOP
	};

	log_line(LOG_INFO, "Setting keydate");
	return mbus_io_acked(fd, frame_out, sizeof(frame_out));
}

int em_read_info(int fd) {
	unsigned char frame_in[256];
	unsigned char frame_out[] = {
		MBUS_FRAME_SHORT_START,
		MBUS_C_REQ_UD2,			/* Control */
		254,				/* Address */
		0x00,				/* Checksum */
		MBUS_FRAME_STOP
	};

	log_line(LOG_INFO, "Reading info");
	ssize_t len = mbus_io(fd, frame_out, sizeof(frame_out), frame_in, sizeof(frame_in));
	if (len < 0) {
		log_line(LOG_ERR, "M-Bus i/o failed: %s", strerror(-len));
		return -len;
	}

	if (mbus_checklong(frame_in, len) < 71) {
		log_line(LOG_ERR, "M-Bus protocol error, received %d unprocessable bytes.", len);
		return EPROTO;
	}

	/* this is nonsense, (very) poor man's DIF/VIF decoding */
	size_t p = MBUS_FRAME_LONG_HDR_LEN + MBUS_RSPUD12_HDR_LEN;
	for (unsigned int i = 0; ; i++) {
		char buf[64];
		size_t b = 0;
		const unsigned char ldb[] = { 0, 1, 2, 3, 4, 4, 6, 8 };
		unsigned int hasdife = (frame_in[p] & 0x80) >> 7;
		unsigned int hasvife = (frame_in[p + 1 + hasdife] & 0x80) >> 7;
		unsigned int d = p + 1 + hasdife + 1 + hasvife;

		b += sprintf(buf + b, "DIF: %02x", frame_in[p]);
		b += sprintf(buf + b, hasdife ? "-%02x" : "   ", frame_in[p + 1]);
		b += sprintf(buf + b, " VIF: %02x", frame_in[p + hasdife + 1]);
		b += sprintf(buf + b, hasvife ? "-%02x" : "   ", frame_in[p + 1 + hasdife + 1]);
		unsigned int df = (frame_in[p] & 0b1111);
		if (df > 7) break;
		unsigned int l = ldb[df];

		unsigned int sn = (frame_in[p] & 0b1000000) >> 6;
		if (hasdife) sn |= ((frame_in[p + 1] & 0b1111) << 1);
		b += sprintf(buf + b, " SN: %d", sn);

		b += sprintf(buf + b, " RAW: ");
		for (unsigned int j = 0; j < l; j++) b += sprintf(buf + b, "%02x ", frame_in[d + j]);

		if ((df == 4) && (frame_in[p + hasdife + 1] == 0x6d)) {
			b += sprintf(buf + b, " VAL: %04d-%02d-%02d %02d:%02d",
				2000 + (frame_in[d + 2] >> 5 | (frame_in[d + 3] & 0b11110000) >> 1),
				frame_in[d + 3] & 0b1111, frame_in[d + 2] & 0b11111, frame_in[d + 1], frame_in[d]);
		} else if (df == 4) {
			b += sprintf(buf + b, " VAL: %ld", (unsigned long int) frame_in[d + 3] << 24 |
				frame_in[d + 2] << 16 | frame_in[d + 1] << 8 | frame_in[d + 0]);
		} else if ((df == 2) && (frame_in[p + hasdife + 1] == 0x6c)) {
			b += sprintf(buf + b, "       VAL: %04d-%02d-%02d",
				2000 + (frame_in[d] >> 5 | (frame_in[d + 1] & 0b11110000) >> 1),
				frame_in[d + 1] & 0b1111, frame_in[d] & 0b11111);
		}

		log_line(LOG_INFO, "%02d: %s", i, buf);
		p = d + l;
		if (p > frame_in[1]) break;
	}

	log_line(LOG_INFO, "Operation completed successfully");
	return 0;
}

int em_read_highres(int fd) {
	unsigned char frame_in[64];
	unsigned char frame_out[] = {
		MBUS_FRAME_LONG_START,
		8,				/* Frame length */
		8,				/* Frame length */
		MBUS_FRAME_LONG_START,
		MBUS_C_SND_UD,			/* Control */
		254,				/* Address */
		MBUS_CI_DATA_SEND,		/* Control info */
		0x0f, 0x01,			/* DIF, VIF: read high res (0x01) */
		0x00, 0x00, 0x60,		/* Unknown or reserved */
		0x12,				/* Checksum */
		MBUS_FRAME_STOP
	};

	log_line(LOG_INFO, "Reading high resolution");
	ssize_t len = mbus_io(fd, frame_out, sizeof(frame_out), frame_in, sizeof(frame_in));
	if (len < 0) {
		log_line(LOG_ERR, "M-Bus i/o failed: %s", strerror(-len));
		return -len;
	}

	if (mbus_checklong(frame_in, len) < 25) {
		log_line(LOG_ERR, "M-Bus protocol error, received %d unprocessable bytes.", len);
		return EPROTO;
	}

	log_line(LOG_INFO, "EM_HIGHRES_READING: %ld ml", frame_in[MBUS_FRAME_LONG_HDR_LEN + MBUS_RSPUD12_HDR_LEN +3] << 24 |
		frame_in[MBUS_FRAME_LONG_HDR_LEN + MBUS_RSPUD12_HDR_LEN + 2] << 16 |
		frame_in[MBUS_FRAME_LONG_HDR_LEN + MBUS_RSPUD12_HDR_LEN + 1] << 8 |
		frame_in[MBUS_FRAME_LONG_HDR_LEN + MBUS_RSPUD12_HDR_LEN]);

	log_line(LOG_INFO, "Operation completed successfully");
	return 0;
}

int em_read_months(int fd) {
	unsigned char frame_in[256];
	unsigned char frame_out[] = {
		MBUS_FRAME_LONG_START,
		8,				/* Frame length */
		8,				/* Frame length */
		MBUS_FRAME_LONG_START,
		MBUS_C_SND_UD,			/* Control */
		254,				/* Address */
		MBUS_CI_DATA_SEND,		/* Control info */
		0x0f, 0x02,			/* DIF, VIF: read end of months (0x02) */
		0x00, 0x00, 0x60,		/* Unknown or reserved */
		0x00,				/* Checksum */
		MBUS_FRAME_STOP
	};

	for (unsigned int i = 0; i <= 1; i++) {
		log_line(LOG_INFO, "Reading monthly usage (%d)", i);
		if (i == 1) frame_out[MBUS_FRAME_LONG_HDR_LEN + 1] = 0x03;	/* DIF, VIF: read middle of months (0x03) */

		ssize_t len = mbus_io(fd, frame_out, sizeof(frame_out), frame_in, sizeof(frame_in));
		if (len < 0) {
			log_line(LOG_ERR, "M-Bus i/o failed: %s", strerror(-len));
			return -len;
		}

		if (mbus_checklong(frame_in, len) < 111) {
			log_line(LOG_ERR, "M-Bus protocol error, received %d unprocessable bytes.", len);
			return EPROTO;
		}

		for (unsigned int i = 0; i < 15; i++) {
			unsigned int off = MBUS_FRAME_LONG_HDR_LEN + MBUS_RSPUD12_HDR_LEN + i * 6;
			log_line(LOG_INFO, "EM_METER_READING_%04d-%02d-%02d: %d", 2000 + ((frame_in[off + 1] & 0b11111110) >> 1),
				 ((frame_in[off + 1] << 8 | frame_in[off]) & 0b111100000) >> 5, frame_in[off] & 0b11111,
				 frame_in[off + 5] << 24 | frame_in[off + 4] << 16 | frame_in[off + 3] << 8 | frame_in[off + 2] );
		}
	}

	log_line(LOG_INFO, "Operation completed successfully");
	return 0;
}

void em_dump_settings(const unsigned char *data) {
	unsigned char buf[32 + 1];
	log_line(LOG_INFO, "EM_FLAGS: 0x%02x", data[0]);
	log_line(LOG_INFO, "EM_OMSMODE: %d", data[1]);
	log_line(LOG_INFO, "EM_FRAMETYPE: %d", data[2]);
	log_line(LOG_INFO, "EM_INTERVAL: %d s", (data[4] << 8 | data[3]));
	log_line(LOG_INFO, "EM_MONTHS: 0b%s (Dec .. Jan)", bitprint(buf, data[6] << 8 | data[5], 12));
	log_line(LOG_INFO, "EM_WEEKOMS: 0b%s (31 .. 1)",
		 bitprint(buf, data[10] << 24 | data[9] << 16 | data[8] << 8 | data[7], 31));
	log_line(LOG_INFO, "EM_DAYOWS: 0b%s (Sun .. Mon)", bitprint(buf, data[11], 7));
	log_line(LOG_INFO, "EM_HOURS: 0b%s (23 .. 00)", bitprint(buf, data[14] << 16 | data[13] << 8 | data[12], 24));
	log_line(LOG_INFO, "EM_ONDAY: %04d-%02d-%02d (%s)", 2000 + ((data[16] & 0b11111110) >> 1),
		 ((data[16] << 8 | data[15]) & 0b111100000) >> 5, data[15] & 0b11111,
		 (data[0] & EM_ENA_STARTDATE) ? "active" : "inactive");
	log_line(LOG_INFO, "EM_ONVOL: %d l (%s)", (data[18] << 8 | data[17]),
		 (data[0] & EM_ENA_STARTVOL) ? "active" : "inactive");
	log_line(LOG_INFO, "EM_OPYEARS: %d", data[19]);
}

int em_get_params(int fd) {
	unsigned char frame_in[64];
	unsigned char frame_out[] = {
		MBUS_FRAME_LONG_START,
		8,				/* Frame length */
		8,				/* Frame length */
		MBUS_FRAME_LONG_START,
		MBUS_C_SND_UD,			/* Control */
		254,				/* Address */
		MBUS_CI_DATA_SEND,		/* Control info */
		0x0f, 0x04,			/* DIF, VIF: get parameters (0x04) */
		0x00, 0x00, 0x60,		/* Unknown or reserved */
		0x15,				/* Checksum */
		MBUS_FRAME_STOP
	};

	log_line(LOG_INFO, "Getting device parameters");
	ssize_t len = mbus_io(fd, frame_out, sizeof(frame_out), frame_in, sizeof(frame_in));
	if (len < 0) {
		log_line(LOG_ERR, "M-Bus i/o failed: %s", strerror(-len));
		return -len;
	}

	if (mbus_checklong(frame_in, len) < 45) {
		log_line(LOG_ERR, "M-Bus protocol error, received %d unprocessable bytes.", len);
		return EPROTO;
	}

	em_dump_settings(frame_in + MBUS_FRAME_LONG_HDR_LEN + MBUS_RSPUD12_HDR_LEN);

	log_line(LOG_INFO, "Operation completed successfully");
	return 0;
}

int em_set_params(int fd) {
	unsigned char frame_out[] = {
		MBUS_FRAME_LONG_START,
		36,				/* Frame length */
		36,				/* Frame length */
		MBUS_FRAME_LONG_START,
		MBUS_C_SND_UD,			/* Control */
		254,				/* Address */
		MBUS_CI_DATA_SEND,		/* Control info */
		0x0f, 0x81,			/* DIF, VIF: set parameters (0x81) */
		0x00, 0x00, 0x60,		/* Unknown or reserved */
		0x00, 0x00, 0x00, 0x00,		/* Unknown or reserved */
		EM_SET_FLAGS,
		EM_SET_OMSMODE,
		EM_SET_FRAMETYPE,
		(EM_SET_INTERVAL >> 0) & 0xFF,
		(EM_SET_INTERVAL >> 8) & 0xFF,
		(EM_SET_MONTHS >> 0) & 0xFF,
		(EM_SET_MONTHS >> 8) & 0xFF,
		(EM_SET_WEEKOMS >> 0) & 0xFF,
		(EM_SET_WEEKOMS >> 8) & 0xFF,
		(EM_SET_WEEKOMS >> 16) & 0xFF,
		(EM_SET_WEEKOMS >> 24) & 0xFF,
		EM_SET_DAYOWS,
		(EM_SET_HOURS >> 0) & 0xFF,
		(EM_SET_HOURS >> 8) & 0xFF,
		(EM_SET_HOURS >> 16) & 0xFF,
		(((EM_SET_ONDAY_YEAR - 2000) << 9 | EM_SET_ONDAY_MONTH << 5 | EM_SET_ONDAY_DAY) >> 0) & 0xFF,
		(((EM_SET_ONDAY_YEAR - 2000) << 9 | EM_SET_ONDAY_MONTH << 5 | EM_SET_ONDAY_DAY) >> 8) & 0xFF,
		(EM_SET_ONVOL >> 0) & 0xFF,
		(EM_SET_ONVOL >> 8) & 0xFF,
		EM_SET_OPYEARS,
		0x00, 0x00, 0x00, 0x00,		/* Unknown or reserved */
		0x00,				/* Checksum */
		MBUS_FRAME_STOP
	};

	log_line(LOG_INFO, "Setting device parameters");
	em_dump_settings(frame_out + MBUS_FRAME_LONG_HDR_LEN + 9);
	return mbus_io_acked(fd, frame_out, sizeof(frame_out));
}

int em_set_aes(int fd) {
	unsigned char frame_out[] = {
		MBUS_FRAME_LONG_START,
		28,				/* Frame length */
		28,				/* Frame length */
		MBUS_FRAME_LONG_START,
		MBUS_C_SND_UD,			/* Control */
		254,				/* Address */
		MBUS_CI_DATA_SEND,		/* Control info */
		0x0f, 0x83,			/* DIF, VIF: set AES key (0x83) */
		0x00, 0x00, 0x60,		/* Unknown or reserved */
		0x00, 0x00, 0x00, 0x00,		/* Unknown or reserved */
		0x89, 0x4f, 0x9f, 0x34, 0xcd, 0xd1, 0x93, 0x41,	/* Sample key: BC1066EA5BFFDCAB4193D1CD349F4F89 */
		0xab, 0xdc, 0xff, 0x5b, 0xea, 0x66, 0x10, 0xbc,
		0x00,				/* Checksum */
		MBUS_FRAME_STOP
	};

	return 7; /* NOT TESTED, remove if you know what you're doing */

	log_line(LOG_INFO, "Setting AES key");
	return mbus_io_acked(fd, frame_out, sizeof(frame_out));
}

int main(int argc, char *argv[]) {
	int err;
	int ret = 1;
	int serial_fd;

	if ((argc != 2) && (argc != 3)) {
		log_line(LOG_ERR, "Usage: %s <serial port> [get_params|set_params|set_time|set_aes|set_keyday|read_months|read_info]\n", argv[0]);
		goto fail;
	}

	serial_fd = open(argv[1], O_RDWR | O_NOCTTY | O_SYNC | O_NONBLOCK);
	if (serial_fd < 0) {
		log_line(LOG_ERR, "Failed to open serial port '%s': %s", argv[1], strerror(errno));
		goto fail;
	}

	log_line(LOG_INFO, "Setting serial port to 2400 baud 8N1");
	err = serial_interface_attribs(serial_fd, B2400, 0);
	if (err < 0) {
		log_line(LOG_ERR, "Failed to set serial port attribs");
		goto fail_cs;
	}

	mbus_wakeup(serial_fd);
	sleep(MBUS_WAKEUP_TIME);

	log_line(LOG_INFO, "Setting serial port to 2400 baud 8E1");
	err = serial_interface_attribs(serial_fd, B2400, PARENB);
	if (err < 0) {
		log_line(LOG_ERR, "Failed to set serial port attribs");
		goto fail_cs;
	}

	if (argv[2] && !strcmp(argv[2], "set_time")) {
		ret = em_set_time(serial_fd);
	} else if (argv[2] && !strcmp(argv[2], "set_aes")) {
		ret = em_set_aes(serial_fd);
	} else if (argv[2] && !strcmp(argv[2], "set_keyday")) {
		ret = em_set_keyday(serial_fd);
	} else if (argv[2] && !strcmp(argv[2], "set_params")) {
		ret = em_set_params(serial_fd);
	} else if (argv[2] && !strcmp(argv[2], "read_months")) {
		ret = em_read_months(serial_fd);
	} else if (argv[2] && !strcmp(argv[2], "read_info")) {
		ret = em_read_info(serial_fd);
	} else if (argv[2] && !strcmp(argv[2], "read_highres")) {
		ret = em_read_highres(serial_fd);
	} else {
		ret = em_get_params(serial_fd);
	}

 fail_cs:
	close(serial_fd);
 fail:
	return ret;
}
