//
// Contact:
//   Moonlight List (moonlight-list@lists.ximian.com)
//
// Copyright 2009 Novell, Inc.
//
// Permission is hereby granted, free of charge, to any person obtaining
// a copy of this software and associated documentation files (the
// "Software"), to deal in the Software without restriction, including
// without limitation the rights to use, copy, modify, merge, publish,
// distribute, sublicense, and/or sell copies of the Software, and to
// permit persons to whom the Software is furnished to do so, subject to
// the following conditions:
// 
// The above copyright notice and this permission notice shall be
// included in all copies or substantial portions of the Software.
// 
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
// EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
// MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
// NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
// LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
// OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
// WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
//

using Mono;
using System;
using System.Threading;
using System.Collections;
using System.Collections.Generic;
using System.Runtime.InteropServices;

namespace System.Windows.Messaging {

	public sealed class LocalMessageSender : INativeEventObjectWrapper
	{
		static char[] InvalidChars = { ',', ':' };
		public const string Global = "*";

		internal LocalMessageSender (IntPtr raw, bool dropref)
		{
			NativeHandle = raw;
			if (dropref)
				NativeMethods.event_object_unref (raw);
		}

		public LocalMessageSender (string receiverName)
			: this (receiverName, null, false)
		{
		}

		public LocalMessageSender (string receiverName, string receiverDomain)
			: this (receiverName, receiverDomain, true)
		{
		}

		private LocalMessageSender (string receiverName, string receiverDomain, bool checkDomain)
			: this (NativeMethods.local_message_sender_new (receiverName, receiverDomain ?? "*"), true)
		{
			if (receiverName == null)
				throw new ArgumentNullException ("receiverName");
			if (receiverName.Length > 256)
				throw new ArgumentException ("receiverName");

			if (checkDomain) {
				if (receiverDomain == null)
					throw new ArgumentNullException ("receiverDomain");
				if ((receiverDomain.Length > 256) || (receiverDomain.IndexOfAny (InvalidChars) != -1))
					throw new ArgumentException ("receiverDomain");
			}

			this.receiverName = receiverName;
			this.receiverDomain = receiverDomain;
		}

		~LocalMessageSender ()
		{
			Free ();
		}

		void Free ()
		{
			if (free_mapping) {
				free_mapping = false;
				NativeDependencyObjectHelper.FreeNativeMapping (this);
			}
		}

		public void SendAsync (string message)
		{
			NativeMethods.local_message_sender_send_async (NativeHandle, message, (IntPtr)GCHandle.Alloc (null));
		}

		public void SendAsync (string message,
				       object userState)
		{
			NativeMethods.local_message_sender_send_async (NativeHandle, message, (IntPtr)GCHandle.Alloc (userState));
		}

		public string ReceiverDomain {
			get { return receiverDomain; }
		}
		public string ReceiverName {
			get { return receiverName; }
		}

		public event EventHandler<SendCompletedEventArgs> SendCompleted {
			add { RegisterEvent (EventIds.LocalMessageSender_SendCompletedEvent, value, Events.CreateSendCompletedEventArgsEventHandlerDispatcher (value)); }
			remove { UnregisterEvent (EventIds.LocalMessageSender_SendCompletedEvent, value); }
		}

		string receiverDomain;
		string receiverName;

		bool free_mapping;

#region "INativeDependencyObjectWrapper interface"
		IntPtr _native;

		internal IntPtr NativeHandle {
			get { return _native; }
			set {
				if (_native != IntPtr.Zero) {
					throw new InvalidOperationException ("native handle is already set");
				}

				_native = value;

				free_mapping = NativeDependencyObjectHelper.AddNativeMapping (value, this);
			}
		}

		IntPtr INativeEventObjectWrapper.NativeHandle {
			get { return NativeHandle; }
			set { NativeHandle = value; }
		}

		void INativeEventObjectWrapper.MentorChanged (IntPtr mentor_ptr)
		{
		}

		void INativeEventObjectWrapper.OnAttached ()
		{
		}

		void INativeEventObjectWrapper.OnDetached ()
		{
		}

		Kind INativeEventObjectWrapper.GetKind ()
		{
			return Kind.LOCALMESSAGESENDER;
		}

#endregion

		private EventHandlerList EventList = new EventHandlerList ();

		private void RegisterEvent (int eventId, Delegate managedHandler, UnmanagedEventHandler nativeHandler)
		{
			if (managedHandler == null)
				return;

			GDestroyNotify dtor_action = (data) => {
				EventList.RemoveHandler (eventId, managedHandler);
			};

			int token = Events.AddHandler (this, eventId, nativeHandler, dtor_action);
			EventList.AddHandler (eventId, token, managedHandler, nativeHandler, dtor_action);
		}

		private void UnregisterEvent (int eventId, Delegate managedHandler)
		{
			UnmanagedEventHandler nativeHandler = EventList.LookupHandler (eventId, managedHandler);

			if (nativeHandler == null)
				return;

			Events.RemoveHandler (this, eventId, nativeHandler);
		}
	}

}

