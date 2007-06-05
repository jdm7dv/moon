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
#include <string.h>
#include <gtk/gtk.h>
#include <malloc.h>
#include <glib.h>
#include <stdlib.h>
#if AGG
#    include <agg_rendering_buffer.h>
#    include "Agg2D.h"
#else
#    define CAIRO 1
#endif

#include "runtime.h"
#include "transform.h"

#if AGG
struct _SurfacePrivate {
	agg::rendering_buffer rbuf;
	Agg2D *graphics;
};
#endif

void 
base_ref (Base *base)
{
	if (base->refcount & BASE_FLOATS)
		base->refcount = 1;
	else
		base->refcount++;
}

void
base_unref (Base *base)
{
	if (base->refcount == BASE_FLOATS || base->refcount == 1){
		delete base;
	} else
		base->refcount--;
}

void 
collection_add (Collection *collection, void *data)
{
	if (collection->add_fn)
		collection->add_fn (collection, data);
	collection->list = g_slist_append (collection->list, data);
}

void 
collection_remove (Collection *collection, void *data)
{
	if (collection->remove_fn)
		collection->remove_fn (collection, data);

	collection->list = g_slist_remove (collection->list, data);
}

/**
 * item_getbounds:
 * @item: the item to update the bounds of
 *
 * Does this by requesting bounds update to all of its parents. 
 */
void
item_update_bounds (UIElement *item)
{
	double cx1 = item->x1;
	double cy1 = item->y1;
	double cx2 = item->x2;
	double cy2 = item->y2;
	
	item->getbounds ();

	//
	// If we changed, notify the parent to recompute its bounds
	//
	if (item->x1 != cx1 || item->y1 != cy1 || item->y2 != cy2 || item->x2 != cx2){
		if (item->parent != NULL)
			item_update_bounds (item->parent);
	}
}

UIElement::~UIElement ()
{
	if (absolute_xform)
		g_free (absolute_xform);
	if (user_xform)
		g_free (user_xform);
}

void 
item_invalidate (UIElement *item)
{
	double res [6];
	Surface *s = item_get_surface (item);

	if (s == NULL)
		return;

#ifdef DEBUG_INVALIDATE
	printf ("Requesting invalidate for %d %d %d %d\n", 
				    (int) item->x1, (int)item->y1, 
				    (int)(item->x2-item->x1), (int)(item->y2-item->y1));
#endif
	// 
	// Note: this is buggy: why do we need to queue the redraw on the toplevel
	// widget (s->data) and does not work with the drawing area?
	//
	gtk_widget_queue_draw_area ((GtkWidget *)s->drawing_area, 
				    (int) item->x1, (int)item->y1, 
				    (int)(item->x2-item->x1+1), (int)(item->y2-item->y1+1));
}

void
item_set_transform (UIElement *item, double *transform)
{
	item_invalidate (item);

	if (transform == NULL){
		if (item->user_xform)
			free (item->user_xform);
		item->user_xform = NULL;
	} else {
		if (item->user_xform == NULL)
			item->user_xform = g_new (double, 6);
		
		memcpy (item->user_xform, transform, sizeof (double) * 6);
	}

	item->update_xform ();

	item->getbounds ();
	item_invalidate (item);
}

void 
item_set_transform_origin (UIElement *item, Point p)
{
	item_invalidate (item);
	
	item->user_xform_origin = p;

	item->update_xform ();
	item->getbounds ();

	item_invalidate (item);
}

void
UIElement::update_xform ()
{
	cairo_matrix_t *cairo_mat;

	if (parent == NULL || parent->absolute_xform == NULL) {
		if (user_xform == NULL){
			if (absolute_xform != NULL){
				g_free (absolute_xform);
				absolute_xform = NULL;
			}
		} else {
			//
			// If there is a rotation, we need the transform origin
			//
			if (absolute_xform == NULL)
				absolute_xform = g_new (double, 6);

			cairo_mat = (cairo_matrix_t *) absolute_xform;

			if (user_xform [1] != 0 || user_xform [2] != 0){
				Point p = getxformorigin ();

				cairo_matrix_init_translate (cairo_mat, p.x, p.y);
				cairo_matrix_multiply (cairo_mat, (cairo_matrix_t *) user_xform, cairo_mat);
				cairo_matrix_translate (cairo_mat, -p.x, -p.y);
			} else {
				memcpy (absolute_xform, user_xform, sizeof (double) * 6);
			}
		}
	} else {
		// parent has an absolute xform

		if (absolute_xform == NULL)
			absolute_xform = g_new (double, 6);
		cairo_mat = (cairo_matrix_t *) absolute_xform;

		memcpy (absolute_xform, parent->absolute_xform, sizeof (double) * 6);
		Point p = getxformorigin ();
		cairo_matrix_translate (cairo_mat, p.x, p.y);
		cairo_matrix_multiply (cairo_mat, (cairo_matrix_t *) user_xform, cairo_mat);
		cairo_matrix_translate (cairo_mat, -p.x, -p.y);
	}
}

void
UIElement::OnSubPropertyChanged (DependencyProperty *prop, DependencyProperty *subprop)
{
	if (prop == UIElement::RenderTransformProperty) {
		cairo_matrix_t trans;
		Transform *t = item_get_render_transform (this);
		t->GetTransform (&trans);
		item_set_transform (this, (double*)&trans);
	}
}

void
item_set_render_transform (UIElement *item, Transform *transform)
{
	item->SetValue (UIElement::RenderTransformProperty, transform);
}

Transform *
item_get_render_transform (UIElement *item)
{
	Value* v = item->GetValue (UIElement::RenderTransformProperty);
	return v == NULL ? NULL : (Transform*)v->u.dependency_object;
}

Surface *
item_get_surface (UIElement *item)
{
	if (item->flags & Video::IS_SURFACE)
		return (Surface *) item;

	if (item->parent != NULL)
		return item_get_surface (item->parent);

	return NULL;
}

static void
panel_child_add (Collection *col, void *datum)
{
	Panel *panel = (Panel *) col->closure;
	UIElement *item = (UIElement *) datum;

	item->parent = panel;
	panel->getbounds ();
	item_invalidate (panel);
}

static void
panel_child_remove (Collection *col, void *datum)
{
	Panel *panel = (Panel *) col->closure;
	
	item_invalidate (panel);
	panel->getbounds ();
}

Panel::Panel ()
{
	children.Setup (panel_child_add, panel_child_remove, this);
}

void
Panel::render (Surface *s, int x, int y, int width, int height)
{
	GSList *il;
	double actual [6];
	
	for (il = children.list; il != NULL; il = il->next){
		UIElement *item = (UIElement *) il->data;

		item->render (s, x, y, width, height);
	}
}

void
Panel::update_xform ()
{
	UIElement::update_xform ();
	GSList *il;

	for (il = children.list; il != NULL; il = il->next){
		UIElement *item = (UIElement *) il->data;

		item->update_xform ();
	}
}

void
Panel::getbounds ()
{
	bool first = TRUE;
	GSList *il;

	for (il = children.list; il != NULL; il = il->next){
		UIElement *item = (UIElement *) il->data;

		item->getbounds ();
		if (first){
			x1 = item->x1;
			x2 = item->x2;
			y1 = item->y1;
			y2 = item->y2;
			first = FALSE;
			continue;
		} 

		if (item->x1 < x1)
			x1 = item->x1;
		if (item->x2 > x2)
			x2 = item->x2;
		if (item->y1 < y1)
			y1 = item->y1;
		if (item->y2 > y2)
			y2 = item->y2;
	}

	// If we found nothing.
	if (first){
		x1 = y1 = x2 = y2 = 0;
	}
}

void 
panel_child_add (Panel *panel, UIElement *item)
{
	collection_add (&panel->children, item);
}

void 
surface_clear (Surface *s, int x, int y, int width, int height)
{
	static unsigned char n;

	if (x < 0){
		width -= x;
		x = 0;
	}

	if (y < 0){
		height -= y;
		y = 0;
	}
	width = MAX (0, MIN (width, s->width));
	height = MAX (0, MIN (height, s->height));

	int stride = s->width * 4;
	unsigned char *dest = s->buffer + (y * stride) + (x * 4);

	while (height--){
		memset (dest, 70, width * 4);
		dest += stride;
	}
}

void
surface_clear_all (Surface *s)
{
	memset (s->buffer, 0, s->width * s->height * 4);
}

static void
surface_realloc (Surface *s)
{
	if (s->buffer){
		free (s->buffer);
		gdk_pixbuf_unref (s->pixbuf);
	}

	int size = s->width * s->height * 4;
	s->buffer = (unsigned char *) malloc (size);
	surface_clear_all (s);
       
	s->pixbuf = gdk_pixbuf_new_from_data (s->buffer, GDK_COLORSPACE_RGB, TRUE, 8, 
					      s->width, s->height, s->width * 4, NULL, NULL);

	s->cairo_surface = cairo_image_surface_create_for_data (
		s->buffer, CAIRO_FORMAT_ARGB32, s->width, s->height, s->width * 4);

	s->cairo = cairo_create (s->cairo_surface);
}

void 
surface_destroy (Surface *s)
{
	cairo_surface_destroy (s->cairo_surface);
	delete s;
}

gboolean
expose_event_callback (GtkWidget *widget, GdkEventExpose *event, gpointer data)
{
	Surface *s = (Surface *) data;

	if (event->area.x > s->width || event->area.y > s->height)
		return TRUE;

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
	surface_repaint (s, event->area.x, event->area.y, event->area.width, event->area.height);
	gdk_draw_pixbuf (widget->window,
			 NULL, 
			 s->pixbuf,
			 event->area.x, event->area.y, // gint src_x, gint src_y,
			 event->area.x, event->area.y, // gint dest_x, gint dest_y,
			 MIN (event->area.width, s->width),
			 MIN (event->area.height, s->height),
			 GDK_RGB_DITHER_NORMAL,
			 0, 0 // gint x_dither, gint y_dither
			 );
	return TRUE;
}

void
UIElement::set_prop_from_str (const char *prop, const char *value)
{

}

/**
 * This is just a temp setup for debugging/demoing
 **/
void
Canvas::set_prop_from_str (const char *prop, const char *value)
{

}

void
Canvas::render (Surface *s, int x, int y, int width, int height)
{
	GSList *il;
	double actual [6];
	
	for (il = children.list; il != NULL; il = il->next){
		UIElement *item = (UIElement *) il->data;

		item->render (s, x, y, width, height);
	}
}

void 
Canvas::getbounds ()
{
	Panel::getbounds ();
}

Surface *
surface_new (int width, int height)
{
	Surface *s = new Surface ();

	s->drawing_area = gtk_drawing_area_new ();
	gtk_widget_show (s->drawing_area);
	gtk_widget_set_usize (s->drawing_area, width, height);
	s->buffer = NULL;
	s->flags |= UIElement::IS_SURFACE;
	s->width = width;
	s->height = height;
	surface_realloc (s);

	gtk_signal_connect (GTK_OBJECT (s->drawing_area), "expose_event",
			    G_CALLBACK (expose_event_callback), s);

	return s;
}

void
surface_repaint (Surface *s, int x, int y, int width, int height)
{
	surface_clear (s, x, y, width, height);
	s->render (s, x, y, width, height);
}

void *
surface_get_drawing_area (Surface *s)
{
	return s->drawing_area;
}

/*
	DependencyObject
*/

GHashTable *DependencyObject::default_values = NULL;

typedef struct {
  DependencyObject *dob;
  DependencyProperty *prop;
} Attachee;

void
DependencyObject::NotifyAttacheesOfPropertyChange (DependencyProperty *subproperty)
{
	for (GSList *l = attached_list; l != NULL; l = l->next){
		Attachee *att = (Attachee*)l->data;

		att->dob->OnSubPropertyChanged (att->prop, subproperty);
	}
}

void
DependencyObject::SetValue (DependencyProperty *property, Value value)
{
	Value *current_value = (Value*)g_hash_table_lookup (current_values, property->name);

	if (current_value == NULL || *current_value != value.k) {
		Value *copy = g_new (Value, 1);
		*copy = value;

		if (current_value != NULL && current_value->k == Value::DEPENDENCY_OBJECT){
			DependencyObject *dob = current_value->u.dependency_object;

			for (GSList *l = dob->attached_list; l; l = l->next) {
				Attachee *att = (Attachee*)l->data;
				if (att->dob == this && att->prop == property) {
					dob->attached_list = g_slist_remove_link (dob->attached_list, l);
					delete att;
					break;
				}
			}
		}

		if (value.k == Value::DEPENDENCY_OBJECT){
			DependencyObject *dob = value.u.dependency_object;
			Attachee *att = new Attachee ();
			att->dob = this;
			att->prop = property;
			dob->attached_list = g_slist_append (dob->attached_list, att);
		}
		g_hash_table_insert (current_values, property->name, copy);

		OnPropertyChanged (property);
	}
}

Value *
DependencyObject::GetValue (DependencyProperty *property)
{
	Value *value = NULL;
	GHashTable * table = NULL;

	value = (Value *) g_hash_table_lookup (current_values, property->name);

	if (value != NULL)
		return value;

	if (default_values == NULL)
		return NULL;

	table = (GHashTable*) g_hash_table_lookup (default_values, &property->type);
	
	if (table == NULL)
		return NULL;

	value = (Value *) g_hash_table_lookup (table, property->name);

	return value;
}

DependencyObject::DependencyObject ()
{
	this->objectType = INVALID;
	current_values = g_hash_table_new_full (g_str_hash, g_str_equal, NULL, g_free);
	events = new EventObject ();
	this->attached_list = NULL;
}

DependencyObject::~DependencyObject ()
{
	g_hash_table_destroy (current_values);
	delete events;
}

void
DependencyObject::SetObjectType (Type objectType)
{
	this->objectType = objectType;
}

DependencyProperty *
DependencyObject::GetDependencyProperty (char *name)
{
	return DependencyObject::GetDependencyProperty (objectType, name);
}

DependencyProperty *
DependencyObject::GetDependencyProperty (DependencyObject::Type type, char *name)
{
	GHashTable *table;
	DependencyProperty *property;

	if (default_values == NULL)
		return NULL;

	table = (GHashTable*) g_hash_table_lookup (default_values, &type);

	if (table == NULL)
		return NULL;

	property = (DependencyProperty*) g_hash_table_lookup (table, name);

	return property;	
}

//
// DependencyObject takes ownership of the Value * for default_value
//
DependencyProperty *
DependencyObject::Register (DependencyObject::Type type, char *name, Value *default_value)
{
	if (NULL == default_values)
		default_values = g_hash_table_new (g_int_hash, g_int_equal);

	DependencyProperty *property = new DependencyProperty (name, default_value);
	property->type = type;
	
	GHashTable *table;

	table = (GHashTable*) g_hash_table_lookup (default_values, &property->type);

	if (table == NULL) {
		table = g_hash_table_new (g_str_hash, g_str_equal);
		g_hash_table_insert (default_values, &property->type, table);
	}

	g_hash_table_insert (table, property->name, property->default_value);

	return property;
}

/*
	DependencyProperty
*/
DependencyProperty::DependencyProperty (char *name, Value *default_value)
{
	this->name = g_strdup (name);
	this->default_value = default_value;
}

DependencyProperty::~DependencyProperty ()
{
	g_free (name);
	if (default_value != NULL)
		g_free (default_value);
}

// event handlers for c++
typedef struct {
	EventHandler func;
	gpointer data;
} EventClosure;

EventObject::EventObject ()
{
	event_hash = g_hash_table_new (g_str_hash, g_str_equal);
}

static void
free_closure_list (gpointer key, gpointer data, gpointer userdata)
{
	g_free (key);
	g_list_foreach ((GList*)data, (GFunc)g_free, NULL);
}

EventObject::~EventObject ()
{
	g_hash_table_foreach (event_hash, free_closure_list, NULL);
}

void
EventObject::AddHandler (char *event_name, EventHandler handler, gpointer data)
{
	GList *events = (GList*)g_hash_table_lookup (event_hash, event_name);

	EventClosure *closure = new EventClosure ();
	closure->func = handler;
	closure->data = data;

	if (events == NULL) {
		g_hash_table_insert (event_hash, g_strdup (event_name), g_list_prepend (NULL, closure));
	}
	else {
		events = g_list_append (events, closure); // not prepending means we don't need to g_hash_table_replace
	}
}

void
EventObject::RemoveHandler (char *event_name, EventHandler handler, gpointer data)
{
	GList *events = (GList*)g_hash_table_lookup (event_hash, event_name);

	if (events == NULL)
		return;

	GList *l;
	for (l = events; l; l = l->next) {
		EventClosure *closure = (EventClosure*)l->data;
		if (closure->func == handler && closure->data == data)
			break;
	}

	if (l == NULL) /* we didn't find it */
		return;

	g_free (l->data);
	events = g_list_delete_link (events, l);

	if (events == NULL) {
		/* delete the event */
		gpointer key, value;
		g_hash_table_lookup_extended (event_hash, event_name, &key, &value);
		g_free (key);
	}
	else {
		g_hash_table_replace (event_hash, event_name, events);
	}
}

void
EventObject::Emit (char *event_name)
{
	GList *events = (GList*)g_hash_table_lookup (event_hash, event_name);

	if (events == NULL)
		return;
	
	for (GList *l = events; l; l = l->next) {
		EventClosure *closure = (EventClosure*)l->data;
		closure->func (closure->data);
	}
}

DependencyProperty* Canvas::TopProperty;
DependencyProperty* Canvas::LeftProperty;

void 
canvas_init ()
{
	Canvas::TopProperty = DependencyObject::Register (DependencyObject::CANVAS, "Top", new Value (0.0));
	Canvas::LeftProperty = DependencyObject::Register (DependencyObject::CANVAS, "Left", new Value (0.0));
}

DependencyProperty* UIElement::RenderTransformProperty;

void
item_init ()
{
	UIElement::RenderTransformProperty = DependencyObject::Register (DependencyObject::UIELEMENT, "RenderTransform", NULL);
}

void
runtime_init ()
{
	item_init ();
	canvas_init ();
	transform_init ();
	animation_init ();
	shape_init ();
	geometry_init ();
}
