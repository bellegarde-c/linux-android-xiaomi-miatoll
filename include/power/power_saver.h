/*
 * Copyright (c) 2024 Cédric Bellegarde <cedric.bellegarde@adishatz.org>
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#ifndef _POWER_SAVER_H_
#define _POWER_SAVER_H_
struct power_saver_drv {
	struct notifier_block cpu_notif;
	struct notifier_block msm_drm_notif;
	wait_queue_head_t update_waitq;
	bool screen_on;
	unsigned int streams;
	void (*sound_enabled)(void);
	void (*sound_disabled)(void);
};

extern struct power_saver_drv power_saver;

#endif				/* _POWER_SAVER_H_ */
