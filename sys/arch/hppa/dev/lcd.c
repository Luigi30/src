/*	$OpenBSD: lcd.c,v 1.5 2020/03/06 01:45:32 cheloha Exp $	*/

/*
 * Copyright (c) 2007 Mark Kettenis
 *
 * Permission to use, copy, modify, and distribute this software for any
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

#include <sys/param.h>
#include <sys/device.h>
#include <sys/kernel.h>
#include <sys/systm.h>
#include <sys/timeout.h>
#include <sys/ioctl.h>
#include <sys/fcntl.h>

#include <machine/autoconf.h>
#include <machine/bus.h>
#include <machine/cpu.h>
#include <machine/pdc.h>
#include <machine/lcd.h>

#define CHARACTER_SLOTS 32

#define LCD_CLS		0x01
#define LCD_HOME	0x02
#define LCD_LOCATE(X, Y)	(((Y) & 1 ? 0xc0 : 0x80) | ((X) & 0x0f))

struct lcd_softc {
	struct device		sc_dv;

	bus_space_tag_t		sc_iot;
	bus_space_handle_t 	sc_cmdh, sc_datah;

	u_int			sc_delay;
	u_int8_t		sc_heartbeat[3];

	struct timeout		sc_to;
	int			sc_on;
	struct blink_led	sc_blink;

        int                     sc_opened;
};

int	lcd_match(struct device *, void *, void *);
void	lcd_attach(struct device *, struct device *, void *);

struct cfattach lcd_ca = {
	sizeof(struct lcd_softc), lcd_match, lcd_attach
};

struct cfdriver lcd_cd = {
        NULL, "lcd", DV_DULL, 0
};

struct lcd_softc *panel;

void	lcd_mountroot(struct device *);
void	lcd_print(struct lcd_softc *, const char *);
void	lcd_blink(void *, int);
void	lcd_blink_finish(void *);

/* Command handlers */
void    lcd_clear(void *);
void    lcd_home(void *);

/* Device routines */
int
lcd_match(struct device *parent, void *match, void *aux)
{
	struct confargs *ca = aux;

	if (strcmp(ca->ca_name, "lcd") == 0)
		return (1);

	return (0);
}

void
lcd_attach(struct device *parent, struct device *self, void *aux)
{
	struct lcd_softc *sc = (struct lcd_softc *)self;
	panel = (struct lcd_softc *)self;
        struct confargs *ca = aux;
	struct pdc_chassis_lcd *pdc_lcd = (void *)ca->ca_pdc_iodc_read;
	int i;

	sc->sc_iot = ca->ca_iot;
	if (bus_space_map(sc->sc_iot, pdc_lcd->cmd_addr,
		1, 0, &sc->sc_cmdh)) {
		printf(": cannot map cmd register\n");
		return;
	}

	if (bus_space_map(sc->sc_iot, pdc_lcd->data_addr,
		1, 0, &sc->sc_datah)) {
		printf(": cannot map data register\n");
		bus_space_unmap(sc->sc_iot, sc->sc_cmdh, 1);
		return;
	}

	printf(": model %d\n", pdc_lcd->model);

	sc->sc_delay = pdc_lcd->delay;
	for (i = 0; i < 3; i++)
		sc->sc_heartbeat[i] = pdc_lcd->heartbeat[i];

	timeout_set(&sc->sc_to, lcd_blink_finish, sc);

	sc->sc_blink.bl_func = lcd_blink;
	sc->sc_blink.bl_arg = sc;
	blink_led_register(&sc->sc_blink);

	config_mountroot(self, lcd_mountroot);
}

void
lcd_mountroot(struct device *self)
{
	struct lcd_softc *sc = (struct lcd_softc *)self;

	bus_space_write_1(sc->sc_iot, sc->sc_cmdh, 0, LCD_CLS);
	delay(100 * sc->sc_delay);

	bus_space_write_1(sc->sc_iot, sc->sc_cmdh, 0, LCD_LOCATE(0, 0));
	delay(sc->sc_delay);

	lcd_print(sc, "OpenBSD/" MACHINE);
}

/*
 *  open/close/write/ioctl
 */
int
lcdopen(dev_t dev, int flags, int fmt, struct proc *pc)
{
	printf("lcdopen\n");
	struct lcd_softc *sc;
	if((sc = lcd_cd.cd_devs[0]) == NULL)
		return ENXIO;
	if(sc->sc_opened)
		return EBUSY;

	panel->sc_opened = 1;

	return 0;
}

int
lcdclose(dev_t dev, int flags, int fmt, struct proc *p)
{
	struct lcd_softc *sc = lcd_cd.cd_devs[0];

	printf("lcdclose\n");
	sc->sc_opened = 0;

	return 0;
}

int
lcdwrite(dev_t dev, struct uio *uio, int flag)
{
	int error;
	size_t len;
	char buf[CHARACTER_SLOTS];

	struct lcd_softc *sc = lcd_cd.cd_devs[0];
	if(sc == NULL)
		return EIO;

	printf("lcdwrite to sc %p\n", sc);

	memset(buf, 0, CHARACTER_SLOTS);

	len = uio->uio_resid;

	if(len > CHARACTER_SLOTS)
		return EIO;

	error = uiomove(buf, len, uio);
	if(error)
		return EIO;

	lcd_print(sc, buf);

	return 0;
}

int
lcdioctl(dev_t dev, u_long cmd, caddr_t addr, int flag, struct proc *p)
{
	/* Check for write permission on a write command. */
	printf("lcdioctl\n");

	/* TODO: Implement ioctl commands! */

	/* Check write access for write commands. */
	switch(cmd) {
	case LCDCLS:
	case LCDHOME:
		if ((flag & FWRITE) == 0)
			return EACCES;
		break;
	}

	struct lcd_softc *sc = lcd_cd.cd_devs[0];

	switch(cmd) {
	case LCDCLS:
		lcd_clear(sc);
		break;

	case LCDHOME:
		lcd_home(sc);
		break;

	default:
		return ENOTTY;
	}

	return ENOTTY;
}

/* internal */


void
lcd_print(struct lcd_softc *sc, const char *str)
{
	printf("lcd_print: Writing string %s to sc %p\n", str, sc);
	while (*str) {
		if(*str == LCD_CLS)
		{
			lcd_clear(sc);
			str++;
		}
		else if(*str == LCD_HOME)
		{
			lcd_home(sc);
			str++;
		}
		else
		{
			bus_space_write_1(sc->sc_iot, sc->sc_datah, 0, *str++);
			delay(sc->sc_delay);
		}
	}
}

void
lcd_blink(void *v, int on)
{
	struct lcd_softc *sc = v;

	sc->sc_on = on;
	bus_space_write_1(sc->sc_iot, sc->sc_cmdh, 0, sc->sc_heartbeat[0]);
	timeout_add_usec(&sc->sc_to, sc->sc_delay);
}

void
lcd_blink_finish(void *v)
{
	struct lcd_softc *sc = v;
	u_int8_t data;

	if (sc->sc_on)
		data = sc->sc_heartbeat[1];
	else
		data = sc->sc_heartbeat[2];

	bus_space_write_1(sc->sc_iot, sc->sc_datah, 0, data);
}

/* Command helpers */
void
lcd_clear(void *v)
{
	struct lcd_softc *sc = v;

	bus_space_write_1(sc->sc_iot, sc->sc_cmdh, 0, LCD_CLS);
	delay(100 * sc->sc_delay);
}

void
lcd_home(void *v)
{
	struct lcd_softc *sc = v;

	bus_space_write_1(sc->sc_iot, sc->sc_cmdh, 0, LCD_LOCATE(0, 0));
	delay(sc->sc_delay);
}
