// This file was generated by the Gtk# code generator.
// Any changes made will be lost if regenerated.

namespace Gst.RtspServer {

	using System;

	public delegate void MediaConstructedHandler(object o, MediaConstructedArgs args);

	public class MediaConstructedArgs : GLib.SignalArgs {
		public Gst.RtspServer.RTSPMedia Object{
			get {
				return (Gst.RtspServer.RTSPMedia) Args [0];
			}
		}

	}
}