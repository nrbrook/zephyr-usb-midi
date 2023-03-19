# zephyr-usb-midi

This is a [Zephyr](https://zephyrproject.org/) implementation of the [USB MIDI 1.0 device class](https://www.usb.org/sites/default/files/midi10.pdf), which allows a device running Zephyr to send and receive [MIDI](https://en.wikipedia.org/wiki/MIDI) data over USB.

Delivers full MIDI messages. No running status.

To facilitate testing and reuse in other projects, the code for parsing, encoding and decoding USB MIDI packets is completely self contained and has no external dependencies. 

## Usage

REFERENCE ZEPHYR MODULE. SEE SAMPLE APP CMakeLists.txt.

The public API is defined in [usb_midi.h](src/usb_midi/usb_midi.h).

## Sample application

3 outputs, 2 inputs.

### LEDs

* __LED 1__ - Turns on when BLE MIDI is available.
* __LED 2__ - Toggles on/off when receiving sysex messages.
* __LED 3__ - Toggles on/off when receiving non-sysex messages with cable number 0.
* __LED 4__- Toggles on/off when receiving non-sysex messages with cable number 1.

### Buttons

* __Button 1__ - Send note on/off on cable number 0.
* __Button 2__ - Send note on/off on cable number 1.
* __Button 3__ - Send note on/off on cable number 2.
* __Button 4__ - Send a streaming sysex message on cable number 0.

## Configuration options

* `CONFIG_USB_MIDI_NUM_INPUTS` - The number of jacks through which MIDI data flows into the device. Between 0 and 16 (inclusive). Defaults to 1.
* `CONFIG_USB_MIDI_NUM_OUTPUTS` - The number of jacks through which MIDI data flows out of the device. Between 0 and 16 (inclusive). Defaults to 1.
* `CONFIG_USB_MIDI_USE_CUSTOM_JACK_NAMES` - Set to `y` to use custom input and output jack names defined by the options below.
* `CONFIG_USB_MIDI_INPUT_JACK_n_NAME` - the name of input jack `n`, where `n` is the cable number of the jack.
* `CONFIG_USB_MIDI_OUTPUT_JACK_n_NAME` - the name of output jack `n`, where `n` is the cable number of the jack.

## Custom jack names

### macOS

The images below show how custom jack names appear in [MidiKeys](https://flit.github.io/projects/midikeys/) on macOS.

<img src="images/macos_input_names.png" width="300"> <img src="images/macos_output_names.png" width="300">

By using the same name for both jacks in an input/ouput pair, MIDI Studio on macOS will display them as a port with that name. For example, these jack names

```
CONFIG_USB_MIDI_INPUT_JACK_0_NAME="Custom port name 1"
CONFIG_USB_MIDI_OUTPUT_JACK_0_NAME="Custom port name 1"
CONFIG_USB_MIDI_INPUT_JACK_1_NAME="Custom port name 2"
CONFIG_USB_MIDI_OUTPUT_JACK_1_NAME="Custom port name 2"
```

result in these port names

<img src="images/macos_port_names.png" width="500">

## USB MIDI device topology

The [USB MIDI 1.0 spec](https://www.usb.org/sites/default/files/midi10.pdf) allows for a wide range of device topologies, i.e ways to interconnect a number of enteties to form a description of a USB MIDI device. Ideally, this description should match the functionality of the physical device.

The spec is thin on topology examples and only provides one for a MIDI adapter (see appendix B) with external input and output jacks that are connected to embedded ouput and input jacks respectively. This topology seems fairly common in open source implementations, but does not seem ideal for devices without without physical MIDI jacks. This implementation uses embedded input and output jacks connected to an element entity corresponding to the device, assuming the most common device type is one without physical jacks.

It is not entirely clear to me how the device topology (beyond the embedded jacks) is reflected in various hosts. If you have any examples, I'd be interested to know. Feel free to [post them in an issue](https://github.com/stuffmatic/zephyr-usb-midi/issues/new).

## Development notes

Development work so far has been done on macOS using [nRF Connect SDK](https://www.nordicsemi.com/Products/Development-software/nRF-Connect-SDK) v2.1.2 and an [nRF52840 DK board](https://www.nordicsemi.com/Products/Development-hardware/nRF52840-DK).

open source macOS apps have been useful during development:

* [MIDI Monitor](https://www.snoize.com/midimonitor/)  
* [MidiKeys](https://flit.github.io/projects/midikeys/)
* [SysEx Librarian](https://www.snoize.com/sysexlibrarian/) 

[USB in a nutshell](https://beyondlogic.org/usbnutshell/usb1.shtml)

On macOS, the MIDI Studio window of the built in Audio MIDI Setup app is useful for inspecting a connected device and making sure it is properly enumrated. ⚠️ __Warning:__ MIDI Studio seems to cache device info between connections, which means changes to device name, port configuration etc won't show up unless you disconnect the device, remove the dimmed device box and reconnect the device.
