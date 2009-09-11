/*	channels.h
	Copyright (C) 2006 Dmitry Groshev and Mark Tyler

	This file is part of mtPaint.

	mtPaint is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation; either version 2 of the License, or
	(at your option) any later version.

	mtPaint is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with mtPaint in the file COPYING.
*/

#include <gtk/gtk.h>


void pressed_channel_create();
void pressed_channel_delete();
void pressed_channel_edit( GtkMenuItem *menu_item, gpointer user_data, gint item );
void pressed_channel_disable( GtkMenuItem *menu_item, gpointer user_data, gint item );
void pressed_channel_alpha_overlay( GtkMenuItem *menu_item );
void pressed_channel_config_overlay();

