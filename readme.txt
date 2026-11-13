HDHomeRun_TUI 0.9.0

Written by Gemini and Claude, prompted by Mark J. Colombo of RabbitEars.info.
Posted to GitHub with permission by Nick Kelsey of SiliconDust.

Licensed under the GPL v3.0.

This is designed to replicate the HDHomeRun GUI on the Linux command line, as
the HDHomeRun GUI is no longer supported on Linux.  This code implements the 
features of the HDHomeRun GUI, but also adds a few other useful functions, 
such as an ability to easily capture stream samples.

To use it, just extract and then launch from the command line:

./hdhomerun_tui

If you need to rebuild, a Makefile is included.

The controls are as follows:

Up/Dn Arrows : Select tuner to view.
r            : Refresh the device list.
i            : Manually add a device by IP address.
m            : Change the tuner's channel map.
Lf/Rt Arrows : Change channel.
PgUp/PgDn    : Scroll status panel if content overflows.
+/- Keys     : Seek for next/previous active channel.
c            : Manually tune to a channel/frequency.
v            : View stream in VLC (select program for ATSC 1.0).
p            : Set the tuned ATSC 3.0 PLPs.
d (ATSC 3.0) : Show detailed PLP information and SNR requirements.
s (ATSC 1.0) : Save a 30-second transport stream capture.
s (ATSC 3.0) : Save a 30-second debug capture.
a (ATSC 1.0) : Save a 30-second TS capture with error checking.
a (ATSC 3.0) : Save a 30-second DBG capture with error checking.
x (ATSC 3.0) : Save a 30-second PCAP capture, if supported.
z (ATSC 3.0) : Save a 30-second PCAP capture with error checking.
Backspace    : During a save, press Backspace to abort.
h            : Show the help screen with this information.
q            : Quit the application.
