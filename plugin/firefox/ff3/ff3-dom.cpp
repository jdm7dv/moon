// define this here so that protypes.h isn't included (and doesn't
// muck with our npapi.h)
#define NO_NSPR_10_SUPPORT

#include "plugin.h"

#include "ff3-bridge.h"

#include <nsCOMPtr.h>
#include <nsIDOMElement.h>
#include <nsIDOMRange.h>
#include <nsIDOMDocumentRange.h>
#include <nsIDOMDocument.h>
#include <nsIDOMWindow.h>
#include <nsStringAPI.h>

// Events
#include <nsIDOMEvent.h>
#include <nsIDOMMouseEvent.h>
#include <nsIDOMKeyEvent.h>
#include <nsIDOMEventTarget.h>
#include <nsIDOMEventListener.h>


#ifdef DEBUG
#define DEBUG_WARN_NOTIMPLEMENTED(x) printf ("not implemented: (%s)\n" G_STRLOC, x)
#define d(x) x
#else
#define DEBUG_WARN_NOTIMPLEMENTED(x)
#define d(x)
#endif

// debug scriptable object
#define ds(x)

#define STR_FROM_VARIANT(v) ((char *) NPVARIANT_TO_STRING (v).utf8characters)

class FF3DomEventWrapper : public nsIDOMEventListener {

	NS_DECL_ISUPPORTS
	NS_DECL_NSIDOMEVENTLISTENER

 public:

	FF3DomEventWrapper () {
		callback = NULL;

		NS_INIT_ISUPPORTS ();
	}

	callback_dom_event *callback;
	nsCOMPtr<nsIDOMEventTarget> target;
	gpointer context;
	NPP npp;
};

struct FF3DomEvent : MoonlightObject {
	FF3DomEvent (NPP instance) : MoonlightObject (instance) { }

	virtual bool GetProperty (int id, NPIdentifier unmapped, NPVariant *result);
	virtual bool Invoke (int id, NPIdentifier name,
			     const NPVariant *args, uint32_t argCount, NPVariant *result);

	nsIDOMEvent * event;
};

NS_IMPL_ISUPPORTS1(FF3DomEventWrapper, nsIDOMEventListener)

NS_IMETHODIMP
FF3DomEventWrapper::HandleEvent (nsIDOMEvent *aDOMEvent)
{
	int client_x, client_y, offset_x, offset_y, mouse_button, key_code, char_code;
	gboolean alt_key, ctrl_key, shift_key;
	nsString str_event;

	if (callback == NULL)
		return NS_OK;

	aDOMEvent->GetType (str_event);

	client_x = client_y = offset_x = offset_y = mouse_button = 0;
	alt_key = ctrl_key = shift_key = FALSE;

	FF3DomEvent *obj = (FF3DomEvent *)
		NPN_CreateObject (npp,
				  FF3DomEventClass);

	obj->event = aDOMEvent;

	nsCOMPtr<nsIDOMMouseEvent> mouse_event = do_QueryInterface (aDOMEvent);
	if (mouse_event != nsnull) {
		int screen_x, screen_y;

		mouse_event->GetScreenX (&screen_x);
		mouse_event->GetScreenY (&screen_y);

		mouse_event->GetClientX (&client_x);
		mouse_event->GetClientY (&client_y);

		offset_x = screen_x - client_x;
		offset_y = screen_y - client_y;

		mouse_event->GetAltKey (&alt_key);
		mouse_event->GetCtrlKey (&ctrl_key);
		mouse_event->GetShiftKey (&shift_key);

		PRUint16 umouse_button;
		mouse_event->GetButton (&umouse_button);
		mouse_button = umouse_button;
	}

	nsCOMPtr<nsIDOMKeyEvent> key_event = do_QueryInterface (aDOMEvent);
	if (key_event != nsnull) {
		PRUint32 ukey_code, uchar_code;

		key_event->GetKeyCode (&ukey_code);
		key_event->GetCharCode (&uchar_code);

		key_code = ukey_code;
		char_code = uchar_code;

		if (char_code == 0 && key_code != 0)
			char_code = key_code;

		key_event->GetAltKey (&alt_key);
		key_event->GetCtrlKey (&ctrl_key);
		key_event->GetShiftKey (&shift_key);
	}

	callback (context, strdup (NS_ConvertUTF16toUTF8 (str_event).get ()), client_x, client_y, offset_x, offset_y,
			alt_key, ctrl_key, shift_key, mouse_button, key_code, char_code, obj);

	return NS_OK;
}

static nsCOMPtr<nsIDOMDocument>
ff3_get_dom_document (NPP npp)
{
	nsCOMPtr<nsIDOMWindow> dom_window;
	NPN_GetValue (npp, NPNVDOMWindow, static_cast<nsIDOMWindow **>(getter_AddRefs(dom_window)));
	if (!dom_window) {
		d (printf ("No DOM window available\n"));
		return NULL;
	}

	nsCOMPtr<nsIDOMDocument> dom_document;
	dom_window->GetDocument (getter_AddRefs (dom_document));
	if (dom_document == nsnull) {
		d(printf ("No DOM document available\n"));
		return NULL;
	}

	return dom_document;
}

FF3BrowserBridge::FF3BrowserBridge ()
{
	FF3DomEventClass = new FF3DomEventType ();
}

const char*
FF3BrowserBridge::HtmlElementGetText (NPP npp, const char *element_id)
{
	nsresult rv = NS_OK;

	nsCOMPtr<nsIDOMDocument> document;
	document = ff3_get_dom_document (npp);
	if (!document)
		return NULL;

	nsString ns_id = NS_ConvertUTF8toUTF16 (element_id, strlen (element_id));
	nsCOMPtr<nsIDOMElement> element;
	rv = document->GetElementById (ns_id, getter_AddRefs (element));
	if (NS_FAILED (rv) || element == NULL)
		return NULL;

	nsCOMPtr<nsIDOMDocument> owner;
	element->GetOwnerDocument (getter_AddRefs (owner));
	
	nsCOMPtr<nsIDOMDocumentRange> doc_range = do_QueryInterface (owner);
	if (!doc_range)
		return NULL;

	nsCOMPtr<nsIDOMRange> range;
	doc_range->CreateRange (getter_AddRefs (range));
	if (!range)
		return NULL;

	range->SelectNodeContents (element);

	nsString text;
	range->ToString (text);
	return g_strdup (NS_ConvertUTF16toUTF8 (text).get ());
}

gpointer
FF3BrowserBridge::HtmlObjectAttachEvent (NPP npp, NPObject *npobj, const char *name, callback_dom_event cb, gpointer context)
{
	nsresult rv;
	NPVariant npresult;
	NPIdentifier id_identifier = NPN_GetStringIdentifier ("id");
	nsCOMPtr<nsISupports> item;

	NPN_GetProperty (npp, npobj, id_identifier, &npresult);

	if (NPVARIANT_IS_STRING (npresult) && strlen (STR_FROM_VARIANT (npresult)) > 0) {
		NPString np_id = NPVARIANT_TO_STRING (npresult);

		nsString ns_id = NS_ConvertUTF8toUTF16 (np_id.utf8characters, strlen (np_id.utf8characters));
		nsCOMPtr<nsIDOMDocument> dom_document = ff3_get_dom_document (npp);

		nsCOMPtr<nsIDOMElement> element;
		rv = dom_document->GetElementById (ns_id, getter_AddRefs (element));
		if (NS_FAILED (rv) || element == nsnull) {
			return NULL;
		}

		item = element;
	} else {
		NPObject *window = NULL;
		NPN_GetValue (npp, NPNVWindowNPObject, &window);

		if (window && npobj->_class == window->_class) {
			NPN_GetValue (npp, NPNVDOMWindow, static_cast<nsISupports **>(getter_AddRefs (item)));
		} else {
			NPVariant docresult;
			NPIdentifier document_identifier = NPN_GetStringIdentifier ("document");

			NPN_GetProperty (npp, window, document_identifier, &docresult);

			if (npobj == NPVARIANT_TO_OBJECT (docresult)) {
				item = ff3_get_dom_document (npp);
			} else {
				const char *temp_id = "__moonlight_temp_id";
				NPVariant npvalue;

				string_to_npvariant (temp_id, &npvalue);
				NPN_SetProperty (npp, npobj, id_identifier, &npvalue);
				NPN_ReleaseVariantValue (&npvalue);

				nsString ns_id = NS_ConvertUTF8toUTF16 (temp_id, strlen (temp_id));
				nsCOMPtr<nsIDOMDocument> dom_document = ff3_get_dom_document (npp);

				nsCOMPtr<nsIDOMElement> element;
				dom_document->GetElementById (ns_id, getter_AddRefs (element));
				if (element == nsnull) {
					d(printf ("Unable to find temp_id element\n"));
					return NULL;
				}

				item = element;

				// reset to it's original empty value
				NPN_SetProperty (npp, npobj, id_identifier, &npresult);
			}
		}
	}

	nsCOMPtr<nsIDOMEventTarget> target = do_QueryInterface (item);

	FF3DomEventWrapper *wrapper = new FF3DomEventWrapper ();
	wrapper->callback = cb;
	wrapper->target = target;
	wrapper->context = context;
	wrapper->npp = npp;

	rv = target->AddEventListener (NS_ConvertUTF8toUTF16 (name, strlen (name)), wrapper, PR_TRUE);

	return wrapper;
}

void
FF3BrowserBridge::HtmlObjectDetachEvent (NPP instance, const char *name, gpointer listener_ptr)
{
	FF3DomEventWrapper *wrapper = (FF3DomEventWrapper *) listener_ptr;

	wrapper->target->RemoveEventListener (NS_ConvertUTF8toUTF16 (name, strlen (name)), wrapper, PR_TRUE);
	wrapper->callback = NULL;
	delete wrapper;
}

static NPObject *
dom_event_allocate (NPP instance, NPClass *klass)
{
	return new FF3DomEvent (instance);
}


FF3DomEventType::FF3DomEventType ()
{
	allocate = dom_event_allocate;
	AddMapping (dom_event_mapping, G_N_ELEMENTS (dom_event_mapping));
}

FF3DomEventType *FF3DomEventClass;


bool
FF3DomEvent::GetProperty (int id, NPIdentifier name, NPVariant *result)
{
	NULL_TO_NPVARIANT (*result);

#if ds(!)0
	NPUTF8 *strname = NPN_UTF8FromIdentifier (name);
	printf ("getting event property %s\n", strname);
	NPN_MemFree (strname);
#endif

	switch (id) {
		case MoonId_Detail: {
			nsCOMPtr<nsIDOMUIEvent> uievent = do_QueryInterface (event);
			if (uievent) {
				int detail;
				uievent->GetDetail (&detail);
				INT32_TO_NPVARIANT (detail, *result);
			}
			return true;
		}
	}

	return false;
}

bool
FF3DomEvent::Invoke (int id, NPIdentifier name,
			     const NPVariant *args, uint32_t argCount, NPVariant *result)
{
	NULL_TO_NPVARIANT (*result);
	ds(printf("FF3DomEvent::Invoke\n"));
	switch (id) {
		case MoonId_StopPropagation: {
			if (event) {
				ds(printf("FF3DomEvent::StopPropagation\n"));
				event->StopPropagation ();
			}
			return true;
		}
		case MoonId_PreventDefault: {
			if (event) {
				ds(printf("FF3DomEvent::PreventDefault\n"));
				event->PreventDefault ();
			}
			return true;
		}
		default: {
			return MoonlightObject::Invoke (id, name, args, argCount, result);
		}
	}
}
