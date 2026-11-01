# CASA0014 — RFID-to-Light Interface for Vespera

## Project Overview

This project explores how a familiar everyday object — a university student ID card — can become an interface for interacting with a connected lighting installation.

I developed a physical prototype using an Arduino MKR WiFi 1010 and a PN532 RFID reader. When a student taps their card, the device reads its unique identifier (UID) and transforms it into a repeatable sequence of five colours and durations.

The device provides immediate local feedback through an LED, a NeoPixel and a buzzer. It also sends lighting commands over MQTT to Vespera, a connected lighting installation at University College London (UCL).

The same card generates the same lighting sequence each time, creating a consistent relationship between a physical identity token and a visual output.

## Project Aim

The aim is to investigate how RFID sensing, embedded computing and network communication can turn a simple physical interaction into a shared visual experience.

Rather than displaying personal information, the prototype uses the card's UID as an input to generate a deterministic lighting pattern.


## Hardware Components

The prototype uses the following hardware:

| Component | Purpose |
|---|---|
| Arduino MKR WiFi 1010 | Main microcontroller, responsible for processing RFID inputs and communicating with the Vespera installation over Wi-Fi and MQTT. |
| PN532 RFID reader | Detects student cards and reads their unique identifiers (UIDs) using I2C communication. |
| NeoPixel RGB LED | Displays colour feedback generated from the scanned card. |
| Piezo buzzer | Provides audible feedback during interaction. |
| Jumper wires and connecting components | Connect the electronic components to the Arduino. |
| Repurposed AirPods Pro cardboard box | Houses the prototype and provides a compact physical enclosure. |

### Physical Design

The electronics are housed inside a repurposed AirPods Pro cardboard box. This approach allowed me to create a compact prototype using an existing object rather than fabricating a new enclosure.

The enclosure brings the RFID reader, NeoPixel and buzzer together into a single physical interface. Photographs of the assembled electronics and enclosure are available in the [`images`](images/) folder.
