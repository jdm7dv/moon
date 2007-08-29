/*
 * runtime.cpp: Core surface and canvas definitions.
 *
 * Author:
 *   Miguel de Icaza (miguel@novell.com)
 *
 * Copyright 2007 Novell, Inc. (http://www.novell.com)
 *
 * See the LICENSE file included with the distribution for details.
 * 
 */

#include <config.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <math.h>

#include <gtk/gtk.h>
#include <glib.h>
#define Visual _XVisual
#include <gdk/gdkx.h>
#include <gdk/gdkkeysyms.h>

#include <cairo-xlib.h>
#undef Visual
#include "runtime.h"
#include "canvas.h"
#include "control.h"
#include "color.h"
#include "shape.h"
#include "transform.h"
#include "animation.h"
#include "downloader.h"
#include "frameworkelement.h"
#include "stylus.h"
#include "rect.h"
#include "text.h"
#include "panel.h"
#include "value.h"
#include "namescope.h"
#include "xaml.h"
#include "dirty.h"

//#define DEBUG_INVALIDATE 1
#define DEBUG_REFCNT 0

#define CAIRO_CLIP 0
#define TIME_CLIP 0
#define TIME_REDRAW 1

void
Surface::CreateSimilarSurface ()
{
    cairo_t *ctx = gdk_cairo_create (drawing_area->window);

    if (cairo_xlib){
	cairo_destroy (cairo_xlib);
    }

    cairo_surface_t *xlib_surface = cairo_surface_create_similar (
	   cairo_get_target (ctx), 
	   CAIRO_CONTENT_COLOR_ALPHA,
	   width, height);

    cairo_destroy (ctx);

    cairo_xlib = cairo_create (xlib_surface);
    cairo_surface_destroy (xlib_surface);
}


Surface::Surface(int w, int h)
  : width (w), height (h), buffer (0), pixbuf (NULL),
    using_cairo_xlib_surface(0),
    cairo_buffer_surface (NULL), cairo_buffer(NULL),
    cairo_xlib(NULL), cairo (NULL), transparent(false),
    background_color(NULL),
    cursor (MouseCursorDefault)
{

	drawing_area = gtk_event_box_new ();

	// don't let gtk clear the window we'll do all the drawing.
	gtk_widget_set_app_paintable (drawing_area, TRUE);

	//
	// Set to true, need to change that to FALSE later when we start
	// repainting again.   
	//
	gtk_event_box_set_visible_window (GTK_EVENT_BOX (drawing_area), TRUE);

	gtk_signal_connect (GTK_OBJECT (drawing_area), "size_allocate",
			    G_CALLBACK(drawing_area_size_allocate), this);
	gtk_signal_connect (GTK_OBJECT (drawing_area), "destroy",
			    G_CALLBACK(drawing_area_destroyed), this);

	gtk_widget_add_events (drawing_area, 
			       GDK_POINTER_MOTION_MASK |
			       GDK_POINTER_MOTION_HINT_MASK |
			       GDK_KEY_PRESS_MASK |
			       GDK_KEY_RELEASE_MASK |
			       GDK_BUTTON_PRESS_MASK |
			       GDK_BUTTON_RELEASE_MASK);
	GTK_WIDGET_SET_FLAGS (drawing_area, GTK_CAN_FOCUS);
	//gtk_widget_set_double_buffered (drawing_area, FALSE);

	gtk_widget_show (drawing_area);

	gtk_widget_set_size_request (drawing_area, width, height);
	buffer = NULL;

	toplevel = NULL;
	capture_element = NULL;

	ResizeEvent = RegisterEvent ("Resize");
	FullScreenChangeEvent = RegisterEvent ("FullScreenChange");

	Realloc ();
	
	full_screen = false;
	can_full_screen = false;
}

Surface::~Surface ()
{
	//
	// This removes one source of problems: the unrealize handler is not
	// being called when Mozilla destroys our window, so we remove it here.
	//
	// This is easy to trigger, open two clocks and try to load a different
	// page on one of the pages (that keeps the timer ticking, and eventually
	// kills this
	//
	// There is still another problem: sometimes we are getting:
	//     The error was 'RenderBadPicture (invalid Picture parameter)'.
	//
	// And I have yet to track what causes this, the stack trace is not 
	// very useful
	//
	TimeManager::Instance()->RemoveHandler (TimeManager::Instance()->RenderEvent, render_cb, this);
	TimeManager::Instance()->RemoveHandler (TimeManager::Instance()->UpdateInputEvent, update_input_cb, this);

	if (toplevel) {
		toplevel->unref ();
		toplevel = NULL;
	}

	if (buffer) {
		free (buffer);
		buffer = NULL;
	}

	cairo_destroy (cairo_buffer);
	cairo_buffer = NULL;

	cairo_surface_destroy (cairo_buffer_surface);
	cairo_buffer_surface = NULL;

	if (drawing_area != NULL){
		g_signal_handlers_disconnect_matched (drawing_area,
						      (GSignalMatchType) G_SIGNAL_MATCH_DATA,
						      0, 0, NULL, NULL, this);

		gtk_widget_destroy (drawing_area);
		drawing_area = NULL;
	}
}

bool
Surface::SetMouseCapture (UIElement *capture)
{
	if (capture != NULL && capture_element != NULL)
		return false;

	capture_element = capture;

	if (capture_element == NULL) {
		// generate the proper Enter/Leave events here for the object
		// under the mouse pointer
	}

	return true;
}

void
Surface::SetCursor (MouseCursor new_cursor)
{
	if (new_cursor != cursor) {
		cursor = new_cursor;

		if (drawing_area == NULL)
			return;

		GdkCursor *c = NULL;
		switch (cursor) {
		case MouseCursorDefault:
			c = NULL;
			break;
		case MouseCursorArrow:
			c = gdk_cursor_new (GDK_LEFT_PTR);
			break;
		case MouseCursorHand:
			c = gdk_cursor_new (GDK_HAND1);
			break;
		case MouseCursorWait:
			c = gdk_cursor_new (GDK_WATCH);
			break;
		case MouseCursorIBeam:
			c = gdk_cursor_new (GDK_XTERM);
			break;
		case MouseCursorStylus:
			c = gdk_cursor_new (GDK_CROSSHAIR); // ??
			break;
		case MouseCursorEraser:
			c = gdk_cursor_new (GDK_PENCIL); // ??
			break;
		case MouseCursorNone:
			// XXX nothing yet.  create a pixmap cursor with no pixel data
			break;
		}

		gdk_window_set_cursor (drawing_area->window, c);
	}
}

void
Surface::ConnectEvents ()
{
	gtk_signal_connect (GTK_OBJECT (drawing_area), "expose_event",
			    G_CALLBACK (Surface::expose_event_callback), this);

	gtk_signal_connect (GTK_OBJECT (drawing_area), "motion_notify_event",
			    G_CALLBACK (motion_notify_callback), this);

	gtk_signal_connect (GTK_OBJECT (drawing_area), "enter_notify_event",
			    G_CALLBACK (crossing_notify_callback), this);

	gtk_signal_connect (GTK_OBJECT (drawing_area), "leave_notify_event",
			    G_CALLBACK (crossing_notify_callback), this);

	gtk_signal_connect (GTK_OBJECT (drawing_area), "key_press_event",
			    G_CALLBACK (key_press_callback), this);

	gtk_signal_connect (GTK_OBJECT (drawing_area), "key_release_event",
			    G_CALLBACK (key_release_callback), this);

	gtk_signal_connect (GTK_OBJECT (drawing_area), "button_press_event",
			    G_CALLBACK (button_press_callback), this);

	gtk_signal_connect (GTK_OBJECT (drawing_area), "button_release_event",
			    G_CALLBACK (button_release_callback), this);

	gtk_signal_connect (GTK_OBJECT (drawing_area), "realize",
			    G_CALLBACK (realized_callback), this);

	gtk_signal_connect (GTK_OBJECT (drawing_area), "unrealize",
			    G_CALLBACK (unrealized_callback), this);

	if (GTK_WIDGET_REALIZED (drawing_area)){
		realized_callback (drawing_area, this);
	}
}

void
Surface::Attach (UIElement *element)
{
	bool first = FALSE;

	if (!Type::Find (element->GetObjectType())->IsSubclassOf (Type::CANVAS)) {
		printf ("Unsupported toplevel\n");
		return;
	}
	
	if (toplevel) {
		toplevel->Invalidate ();
		toplevel->unref ();
	} else 
		first = TRUE;

	Canvas *canvas = (Canvas *) element;
	canvas->ref ();

	// make sure we have a namescope at the toplevel so that names
	// can be registered/resolved properly.
	if (NameScope::GetNameScope (canvas) == NULL) {
		NameScope::SetNameScope (canvas, new NameScope());
	}

	canvas->SetSurface (this);
	toplevel = canvas;

	// First time we connect the surface, start responding to events
	if (first)
		ConnectEvents ();

	canvas->OnLoaded ();

	bool change_size = false;
	//
	// If the did not get a size specified
	//
	if (width == 0){
		Value *v = toplevel->GetValue (FrameworkElement::WidthProperty);

		if (v){
			width = (int) v->AsDouble ();
			if (width < 0)
				width = 0;
			change_size = true;
		}
	}

	if (height == 0){
		Value *v = toplevel->GetValue (FrameworkElement::HeightProperty);

		if (v){
			height = (int) v->AsDouble ();
			if (height < 0)
				height = 0;
			change_size = true;
		}
	}

	if (change_size)
		Realloc ();

	canvas->UpdateBounds ();
	canvas->Invalidate ();
}

void
Surface::Invalidate (Rect r)
{
	gtk_widget_queue_draw_area (drawing_area,
				    (int) r.x, (int)r.y, 
				    (int)(r.w+2), (int)(r.h+2));
}

void
Surface::Paint (cairo_t *ctx, int x, int y, int width, int height)
{
	if (is_anything_dirty())
		process_dirty_elements();
	
	toplevel->DoRender (ctx, x, y, width, height);
}

//
// This will resize the surface (merely a convenience function for
// resizing the widget area that we have.
//
// This will not change the Width and Height properties of the 
// toplevel canvas, if you want that, you must do that yourself
//
void
Surface::Resize (int width, int height)
{
	gtk_widget_set_size_request (drawing_area, width, height);

	Emit (ResizeEvent);
}

void
Surface::Realloc ()
{
	if (buffer)
		free (buffer);

	int size = width * height * 4;
	buffer = (unsigned char *) malloc (size);

	cairo_buffer_surface = cairo_image_surface_create_for_data (
		buffer, CAIRO_FORMAT_ARGB32, width, height, width * 4);

	cairo_buffer = cairo_create (cairo_buffer_surface);

	if (cairo_xlib == NULL) {
		cairo = cairo_buffer;
	}
	else {
		CreateSimilarSurface ();
		cairo = cairo_xlib;
	}
}

void
Surface::SetFullScreen (bool value)
{
	if (value && !can_full_screen) {
		g_warning ("You're not allowed to switch to fullscreen from where you're doing it.");
		return;
	}
	
	UpdateFullScreen (value);
	
}

void
Surface::UpdateFullScreen (bool value)
{
	if (value == full_screen)
		return;
	
	if (value) {
		g_warning ("Fullscreen mode not implemented yet.");
	} else {
		g_warning ("Fullscreen mode not implemented yet.");
	}
	full_screen = value;
	Emit (FullScreenChangeEvent);
}

void
Surface::render_cb (EventObject *sender, gpointer calldata, gpointer closure)
{
	Surface *s = (Surface*)closure;
	gdk_window_process_updates (GTK_WIDGET (s->drawing_area)->window, FALSE);
}

void
Surface::update_input_cb (EventObject *sender, gpointer calldata, gpointer closure)
{
	Surface *s = (Surface*)closure;

	MouseCursor new_cursor = MouseCursorDefault;

	UIElement *input_element = s->capture_element ? s->capture_element : s->toplevel;
	input_element->HandleMotion (s->cairo, s->last_event_state, s->last_event_x, s->last_event_y, &new_cursor);
	s->SetCursor (new_cursor);
}

gboolean
Surface::realized_callback (GtkWidget *widget, gpointer data)
{
	Surface *s = (Surface *) data;

	s->CreateSimilarSurface ();
	s->cairo = s->cairo_xlib;

	TimeManager::Instance()->AddHandler (TimeManager::Instance()->RenderEvent, render_cb, s);
	TimeManager::Instance()->AddHandler (TimeManager::Instance()->UpdateInputEvent, update_input_cb, s);
	return TRUE;
}

gboolean
Surface::unrealized_callback (GtkWidget *widget, gpointer data)
{
	Surface *s = (Surface *) data;

	if (s->cairo_xlib) {
		cairo_destroy (s->cairo_xlib);
		s->cairo_xlib = NULL;
	}

	s->cairo = s->cairo_buffer;
	TimeManager::Instance()->RemoveHandler (TimeManager::Instance()->RenderEvent, render_cb, s);
	TimeManager::Instance()->RemoveHandler (TimeManager::Instance()->UpdateInputEvent, update_input_cb, s);
	return TRUE;
}

gboolean
Surface::expose_event_callback (GtkWidget *widget, GdkEventExpose *event, gpointer data)
{
	Surface *s = (Surface *) data;

	s->frames++;

	if (event->area.x > s->width || event->area.y > s->height)
		return TRUE;

#if TIME_REDRAW
	STARTTIMER (expose, "redraw");
#endif
	s->cairo = s->cairo_xlib;

	//
	// BIG DEBUG BLOB
	// 
	if (cairo_status (s->cairo) != CAIRO_STATUS_SUCCESS){
		printf ("expose event: the cairo context has an error condition and refuses to paint: %s\n", 
			cairo_status_to_string (cairo_status (s->cairo)));
	}

#ifdef DEBUG_INVALIDATE
	printf ("Got a request to repaint at %d %d %d %d\n", event->area.x, event->area.y, event->area.width, event->area.height);
#endif

	cairo_t *ctx = gdk_cairo_create (widget->window);
	gdk_cairo_region (ctx, event->region);
	cairo_clip (ctx);

	//
	// These are temporary while we change this to paint at the offset position
	// instead of using the old approach of modifying the topmost Canvas (a no-no),
	//
	// The flag "s->transparent" is here because I could not
	// figure out what is painting the background with white now.
	// The change that made the white painting implicit instead of
	// explicit is patch 80632.   I would appreciate any help in tracking down
	// the proper way of making the background white when not running in 
	// "transparent" mode.    
	//
	// Either exposing surface_set_trans to turn the next code is a hack, 
	// or it is normal to request all code that wants to paint to manually
	// clear the background to white beforehand.    For now am going with
	// making this an explicit surface API.
	//
	// The second part is for coping with the future: when we support being 
	// windowless
	//
	if (s->transparent && !GTK_WIDGET_NO_WINDOW (widget)){
		cairo_set_operator (ctx, CAIRO_OPERATOR_SOURCE);
		cairo_set_source_rgba (ctx, 1, 1, 1, 0);
		cairo_paint (ctx);
	}
	else if (s->background_color) {
		cairo_set_operator (ctx, CAIRO_OPERATOR_SOURCE);
		cairo_set_source_rgba (ctx,
				       s->background_color->r * s->background_color->a,
				       s->background_color->g * s->background_color->a,
				       s->background_color->b * s->background_color->a,
				       s->background_color->a);
		cairo_paint (ctx);
	}

	cairo_set_operator (ctx, CAIRO_OPERATOR_OVER);
	s->Paint (ctx, event->area.x, event->area.y, event->area.width, event->area.height);
	cairo_destroy (ctx);

#if TIME_REDRAW
	ENDTIMER (expose, "redraw");
#endif

	return TRUE;
}

gboolean
Surface::motion_notify_callback (GtkWidget *widget, GdkEventMotion *event, gpointer data)
{
	Surface *s = (Surface *) data;
	GdkModifierType state;
	double x, y;

	if (event->is_hint) {
		int ix, iy;
		gdk_window_get_pointer (event->window, &ix, &iy, &state);
		x = ix;
		y = iy;
	} else {
		x = event->x;
		y = event->y;

		if (GTK_WIDGET_NO_WINDOW (widget)){
			x -= widget->allocation.x;
			y -= widget->allocation.y;
		}

		state = (GdkModifierType)event->state;
	}

	s->last_event_x = x;
	s->last_event_y = y;
	s->last_event_state = state;

	MouseCursor new_cursor = MouseCursorDefault;
	UIElement *input_element = s->capture_element ? s->capture_element : s->toplevel;
	input_element->HandleMotion (s->cairo, state, x, y, &new_cursor);
	s->SetCursor (new_cursor);

	return TRUE;
}

gboolean
Surface::crossing_notify_callback (GtkWidget *widget, GdkEventCrossing *event, gpointer data)
{
	Surface *s = (Surface *) data;

	if (event->type == GDK_ENTER_NOTIFY){
		double x = event->x;
		double y = event->y;

		if (GTK_WIDGET_NO_WINDOW (widget)){
			x -= widget->allocation.x;
			y -= widget->allocation.y;
		}
		
		MouseCursor new_cursor = MouseCursorDefault;

		if (s->capture_element) {
			s->capture_element->HandleMotion (s->cairo, event->state, x, y, &new_cursor);
		}
		else {
		  	s->toplevel->HandleMotion (s->cairo, event->state, x, y, &new_cursor);
			s->toplevel->Enter (s->cairo, event->state, x, y);
		}

		s->SetCursor (new_cursor);

		s->last_event_x = x;
		s->last_event_y = y;
		s->last_event_state = event->state;
	
	} else {
		if (s->capture_element == NULL)
			s->toplevel->Leave ();
	}

	return TRUE;
}

Key
Surface::gdk_keyval_to_key (guint keyval)
{
	switch (keyval) {
	case GDK_BackSpace:				return KeyBACKSPACE;
	case GDK_Tab:					return KeyTAB;
	case GDK_Return: case GDK_KP_Enter:		return KeyENTER;
	case GDK_Shift_L: case GDK_Shift_R:		return KeySHIFT;
	case GDK_Control_L: case GDK_Control_R:		return KeyCTRL;
	case GDK_Alt_L: case GDK_Alt_R:			return KeyALT;
	case GDK_Caps_Lock:				return KeyCAPSLOCK;
	case GDK_Escape:				return KeyESCAPE;
	case GDK_space: case GDK_KP_Space:		return KeySPACE;
	case GDK_Page_Up: case GDK_KP_Page_Up:		return KeyPAGEUP;
	case GDK_Page_Down: case GDK_KP_Page_Down:	return KeyPAGEDOWN;
	case GDK_End: case GDK_KP_End:			return KeyEND;
	case GDK_Home: case GDK_KP_Home:		return KeyHOME;
	case GDK_Left: case GDK_KP_Left:		return KeyLEFT;
	case GDK_Up: case GDK_KP_Up:			return KeyUP;
	case GDK_Right: case GDK_KP_Right:		return KeyRIGHT;
	case GDK_Down: case GDK_KP_Down:		return KeyDOWN;
	case GDK_Insert: case GDK_KP_Insert:		return KeyINSERT;
	case GDK_Delete: case GDK_KP_Delete:		return KeyINSERT;
	case GDK_0:					return KeyDIGIT0;
	case GDK_1:					return KeyDIGIT1;
	case GDK_2:					return KeyDIGIT2;
	case GDK_3:					return KeyDIGIT3;
	case GDK_4:					return KeyDIGIT4;
	case GDK_5:					return KeyDIGIT5;
	case GDK_6:					return KeyDIGIT6;
	case GDK_7:					return KeyDIGIT7;
	case GDK_8:					return KeyDIGIT8;
	case GDK_9:					return KeyDIGIT9;
	case GDK_a: case GDK_A:				return KeyA;
	case GDK_b: case GDK_B:				return KeyB;
	case GDK_c: case GDK_C:				return KeyC;
	case GDK_d: case GDK_D:				return KeyD;
	case GDK_e: case GDK_E:				return KeyE;
	case GDK_f: case GDK_F:				return KeyF;
	case GDK_g: case GDK_G:				return KeyG;
	case GDK_h: case GDK_H:				return KeyH;
	case GDK_i: case GDK_I:				return KeyI;
	case GDK_j: case GDK_J:				return KeyJ;
	case GDK_k: case GDK_K:				return KeyK;
	case GDK_l: case GDK_L:				return KeyL;
	case GDK_m: case GDK_M:				return KeyM;
	case GDK_n: case GDK_N:				return KeyN;
	case GDK_o: case GDK_O:				return KeyO;
	case GDK_p: case GDK_P:				return KeyP;
	case GDK_q: case GDK_Q:				return KeyQ;
	case GDK_r: case GDK_R:				return KeyR;
	case GDK_s: case GDK_S:				return KeyS;
	case GDK_t: case GDK_T:				return KeyT;
	case GDK_u: case GDK_U:				return KeyU;
	case GDK_v: case GDK_V:				return KeyV;
	case GDK_w: case GDK_W:				return KeyW;
	case GDK_x: case GDK_X:				return KeyX;
	case GDK_y: case GDK_Y:				return KeyY;
	case GDK_z: case GDK_Z:				return KeyZ;
	  
	case GDK_F1: case GDK_KP_F1:			return KeyF1;
	case GDK_F2: case GDK_KP_F2:			return KeyF2;
	case GDK_F3: case GDK_KP_F3:			return KeyF3;
	case GDK_F4: case GDK_KP_F4:			return KeyF4;
	case GDK_F5:					return KeyF5;
	case GDK_F6:					return KeyF6;
	case GDK_F7:					return KeyF7;
	case GDK_F8:					return KeyF8;
	case GDK_F9:					return KeyF9;
	case GDK_F10:					return KeyF10;
	case GDK_F11:					return KeyF11;
	case GDK_F12:					return KeyF12;
	  
	case GDK_KP_0:					return KeyNUMPAD0;
	case GDK_KP_1:					return KeyNUMPAD1;
	case GDK_KP_2:					return KeyNUMPAD2;
	case GDK_KP_3:					return KeyNUMPAD3;
	case GDK_KP_4:					return KeyNUMPAD4;
	case GDK_KP_5:					return KeyNUMPAD5;
	case GDK_KP_6:					return KeyNUMPAD6;
	case GDK_KP_7:					return KeyNUMPAD7;
	case GDK_KP_8:					return KeyNUMPAD8;
	case GDK_KP_9:					return KeyNUMPAD9;
	  
	case GDK_KP_Multiply:				return KeyMULTIPLY;
	case GDK_KP_Add:				return KeyADD;
	case GDK_KP_Subtract:				return KeySUBTRACT;
	case GDK_KP_Decimal:				return KeyDECIMAL;
	case GDK_KP_Divide:				return KeyDIVIDE;
	  
	default:
		return KeyKEYUNKNOWN;
	}
}

bool
Surface::FullScreenKeyHandled (GdkEventKey *key)
{
	if (!GetFullScreen ())
		return false;
		
	// If we're in fullscreen mode no key events are passed through.
	// We only handle Esc, to exit fullscreen mode.
	if (key->keyval == GDK_Escape) {
		SetFullScreen (false);
	}
	return true;
}

gboolean 
Surface::key_press_callback (GtkWidget *widget, GdkEventKey *key, gpointer data)
{
	Surface *s = (Surface *) data;

	if (s->FullScreenKeyHandled (key))
		return TRUE;

	s->SetCanFullScreen (true);
	// 
	// I could not write a test that would send the output elsewhere, for now
	// just send to the toplevel
	//
	s->toplevel->HandleKeyDown (s->cairo,
				    key->state, gdk_keyval_to_key (key->keyval), key->hardware_keycode);
				    
	s->SetCanFullScreen (false);
	
	return TRUE;
}

gboolean 
Surface::key_release_callback (GtkWidget *widget, GdkEventKey *key, gpointer data)
{
	Surface *s = (Surface *) data;

	if (s->FullScreenKeyHandled (key))
		return TRUE;

	s->SetCanFullScreen (true);
	
	// 
	// I could not write a test that would send the output elsewhere, for now
	// just send to the toplevel
	//
	s->toplevel->HandleKeyUp (s->cairo,
				    key->state, gdk_keyval_to_key (key->keyval), key->hardware_keycode);
				    				    
	s->SetCanFullScreen (false);
	
	return TRUE;
}

gboolean
Surface::button_release_callback (GtkWidget *widget, GdkEventButton *button, gpointer data)
{
	Surface *s = (Surface *) data;

	if (button->button != 1)
		return FALSE;

	s->SetCanFullScreen (true);
	
	double x = button->x;
	double y = button->y;
	if (GTK_WIDGET_NO_WINDOW (widget)){
		x -= widget->allocation.x;
		y -= widget->allocation.y;
	}
	UIElement *input_element = s->capture_element ? s->capture_element : s->toplevel;
	input_element->HandleButtonRelease (s->cairo, button->state, x, y);
	
	s->SetCanFullScreen (false);
	
	return TRUE;
}

gboolean
Surface::button_press_callback (GtkWidget *widget, GdkEventButton *button, gpointer data)
{
	Surface *s = (Surface *) data;

	gtk_widget_grab_focus (widget);

	if (button->button != 1)
		return FALSE;

	s->SetCanFullScreen (true);
	
	double x = button->x;
	double y = button->y;
	if (GTK_WIDGET_NO_WINDOW (widget)){
		x -= widget->allocation.x;
		y -= widget->allocation.y;
	}
	UIElement *input_element = s->capture_element ? s->capture_element : s->toplevel;
	input_element->HandleButtonPress (s->cairo, button->state, x, y);
	
	s->SetCanFullScreen (false);
	
	return FALSE;
}


void
Surface::drawing_area_size_allocate (GtkWidget *widget, GtkAllocation *allocation, gpointer user_data)
{
	Surface *s = (Surface *) user_data;

        if (s->width != allocation->width || s->height != allocation->height){
                s->width = allocation->width;
                s->height = allocation->height;

		s->Realloc ();
	}
	
	// if x or y changed we need to recompute the presentation matrix
	// because the toplevel position depends on the allocation.
	if (s->toplevel)
		s->toplevel->UpdateBounds ();
}

void
Surface::drawing_area_destroyed (GtkWidget *widget, gpointer data)
{
	Surface *s = (Surface *) data;

	// This is never called, why?
	printf ("------------------ WE ARE DESTROYED ---------------\n");
	s->drawing_area = NULL;
}



Surface *
surface_new (int width, int height)
{
	return new Surface (width, height);
}

void 
surface_destroy (Surface *s)
{
	delete s;
}

void
surface_resize (Surface *s, int width, int height)
{
	s->Resize (width, height);
}

void
surface_attach (Surface *surface, UIElement *toplevel)
{
	surface->Attach (toplevel);
}

void
surface_paint (Surface *s, cairo_t *ctx, int x, int y, int width, int height)
{
	s->Paint (ctx, x, y, width, height);
}

void *
surface_get_drawing_area (Surface *s)
{
	return s->GetDrawingArea ();
}

void
Surface::SetTrans (bool trans)
{
	transparent = trans;
	if (drawing_area)
		gtk_widget_queue_draw (drawing_area);
}

void
Surface::SetBackgroundColor (Color *color)
{
	printf("YO");
	background_color = new Color (*color);
	if (drawing_area)
		gtk_widget_queue_draw (drawing_area);
}

void 
surface_set_trans (Surface *s, bool trans)
{
	s->SetTrans (trans);
}

bool
surface_get_trans (Surface *s)
{
	return s->GetTrans ();
}


cairo_t*
measuring_context_create (void)
{
	cairo_surface_t* surf = cairo_image_surface_create (CAIRO_FORMAT_A1, 1, 1);
	return cairo_create (surf);
}

void
measuring_context_destroy (cairo_t *cr)
{
	cairo_surface_destroy (cairo_get_target (cr));
	cairo_destroy (cr);
}

static bool inited = false;

void
runtime_init (void)
{
	if (inited)
		return;
	
	if (cairo_version () < CAIRO_VERSION_ENCODE(1,4,0)) {
		printf ("*** WARNING ***\n");
		printf ("*** Cairo versions < 1.4.0 should not be used for Moon.\n");
		printf ("*** Moon was configured to use Cairo version %d.%d.%d, but\n", CAIRO_VERSION_MAJOR, CAIRO_VERSION_MINOR, CAIRO_VERSION_MICRO);
		printf ("*** is being run against version %s.\n", cairo_version_string ());
		printf ("*** Proceed at your own risk\n");
	}

	inited = true;

	g_type_init ();

	TimeManager::Instance()->Start();

	types_init ();
	namescope_init ();
	uielement_init ();
	collection_init ();
	framework_element_init ();
	canvas_init ();
	dependencyobject_init();
	event_trigger_init ();
	transform_init ();
	animation_init ();
	brush_init ();
	shape_init ();
	geometry_init ();
	xaml_init ();
	clock_init ();
	text_init ();
	downloader_init ();
	media_init ();
	panel_init ();
	stylus_init ();
}

//
// These are the plugin-less versions of these methods
//
uint32_t
runtime_html_timer_timeout_add (int interval, GSourceFunc callback, gpointer data)
{
	return  g_timeout_add (interval, callback, data);
}

void 
runtime_html_timer_timeout_stop (uint32_t source_id)
{
	g_source_remove (source_id);
}

void
runtime_shutdown ()
{
	if (!inited)
		return;

	TimeManager::Instance()->Shutdown ();
	Type::Shutdown ();
	DependencyObject::Shutdown ();

	inited = false;
	
}
