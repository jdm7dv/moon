//
// DesignerProperties.cs
//
// Contact:
//   Moonlight List (moonlight-list@lists.ximian.com)
//
// Copyright 2008 Novell, Inc.
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

using System.Windows;

namespace System.ComponentModel {
	public static partial class DesignerProperties {

		public static bool GetIsInDesignMode (DependencyObject element)
		{
			if (element == null)
				throw new ArgumentNullException ("element");
			if (element != Application.Current.RootVisual)
				return false;
			return (bool) element.GetValue (IsInDesignModeProperty);
		}

		public static void SetIsInDesignMode (DependencyObject element, bool value)
		{
			if (element == null)
				throw new ArgumentNullException ("element");
			if (element != Application.Current.RootVisual)
				throw new NotImplementedException ();
			element.SetValue (IsInDesignModeProperty, value);
		}

		public static bool IsInDesignTool {
			get { return false; }
			set { }
		}

		public static bool RefreshOnlyXmlnsDefinitionsOnAssemblyReplace {
			get; set;
		}
	}
}
