/*
 * fldesktop audio backend (ported from tde-mixer)
 *
 * Copyright (C) 2026 tde-mixer and fldesktop developers
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef AUDIOBACKEND_H
#define AUDIOBACKEND_H

#include <stdint.h>

#include <map>
#include <string>
#include <vector>

#include <functional>

/*
 * The data model shared by both backends.
 *
 * The backend fills the model from its own thread (pw_thread_loop); the
 * GUI thread gets a deep copy (AudioBackend::model()).
 *
 * Volumes are "perceptual" fractions as PulseAudio presents them:
 * 1.0 is 100 %, 0.5 is what a volume slider shows as 50 %. The PipeWire
 * backend converts to and from PipeWire's linear amplitudes (cubic law,
 * like pipewire-pulse and wpctl).
 */
namespace Audio {

static const uint32_t InvalidId = 0xffffffffu;

enum DeviceKind { Sink = 0, Source = 1 };
enum StreamKind { Playback = 0, Recording = 1 };

struct Port {
	std::string name;
	std::string description;
	bool available;
	Port() : available(true) {}
};

struct Device {
	uint32_t id;
	DeviceKind kind;
	std::string name;           // stable name, e.g. alsa_output.pci-0000_00_1f.3.analog-stereo
	std::string description;    // human readable
	std::string iconName;
	std::string monitorName;    // PulseAudio only: monitor source of a sink
	std::vector<float> volumes; // one per channel
	std::vector<std::string> channels;
	bool muted;
	bool hasVolume;
	bool isMonitor;             // a monitor source (hidden in the input list)
	bool isVirtual;             // not backed by a sound card (null sink, filters, ...)
	std::vector<Port> ports;
	std::string activePort;
	uint32_t cardId;
	Device() : id(InvalidId), kind(Sink), muted(false), hasVolume(false),
	           isMonitor(false), isVirtual(false), cardId(InvalidId) {}
};

struct Stream {
	uint32_t id;
	StreamKind kind;
	std::string appName;
	std::string binary;
	std::string iconName;
	std::string mediaName;
	std::string role;
	uint32_t deviceId;          // the device it plays to / records from
	std::vector<float> volumes;
	std::vector<std::string> channels;
	bool muted;
	bool hasVolume;
	bool corked;                // paused
	Stream() : id(InvalidId), kind(Playback), deviceId(InvalidId), muted(false),
	           hasVolume(false), corked(false) {}
};

struct Profile {
	std::string name;
	std::string description;
	bool available;
	uint32_t priority;
	Profile() : available(true), priority(0) {}
};

struct Card {
	uint32_t id;
	std::string name;
	std::string description;
	std::string iconName;
	std::vector<Profile> profiles;
	std::string activeProfile;
	Card() : id(InvalidId) {}
};

struct Model {
	std::map<uint32_t, Device> devices;
	std::map<uint32_t, Stream> streams;
	std::map<uint32_t, Card> cards;
	std::string defaultSink;    // Device::name of the default output
	std::string defaultSource;  // Device::name of the default input
	std::string serverName;     // e.g. "PulseAudio (on PipeWire 1.4.2)"
	// the programs capturing video through the server (a camera), and whether
	// any video source runs; PipeWire only
	std::vector<std::string> videoUsers;
	bool videoRunning = false;
	bool connected;
	Model() : connected(false) {}
	void clear();
};

/** the highest channel volume, the value a single volume slider shows */
float level(const std::vector<float> &volumes);

/** volumes scaled so that their highest channel is @p level (keeps the balance) */
std::vector<float> scaled(const std::vector<float> &volumes, float level);

} // namespace Audio


/*
 * The backend runs the sound server connection in its own thread. Model
 * updates are copied to the FLTK thread (Fl::awake, so the program must call
 * Fl::lock() once) and reported through on_changed / on_connection.
 */
class AudioBackend
{
public:
	/** Creates and starts the PipeWire backend; 0 if none was built in. */
	static AudioBackend *create();

	AudioBackend();
	virtual ~AudioBackend();

	/** "PipeWire" */
	virtual std::string backendName() const = 0;
	/** connects to the server; false if it cannot be reached at all */
	virtual bool start() = 0;

	/** the latest copy of the model, only valid in the GUI thread */
	const Audio::Model &model() const { return m_model; }
	bool isConnected() const { return m_model.connected; }

	std::vector<const Audio::Device *> devices(Audio::DeviceKind kind, bool includeMonitors = false) const;
	std::vector<const Audio::Stream *> streams(Audio::StreamKind kind) const;
	const Audio::Device *device(uint32_t id) const;
	const Audio::Stream *stream(uint32_t id) const;
	const Audio::Card *card(uint32_t id) const;
	const Audio::Device *defaultDevice(Audio::DeviceKind kind) const;

	/** turns level meters on or off (they cost a little CPU) */
	virtual void setPeaksEnabled(bool enabled);
	bool peaksEnabled() const { return m_peaksEnabled; }
	/** the current peak (0..1) of a device or stream, -1 if not measured */
	float peak(uint32_t id);

	virtual void setDeviceVolume(uint32_t id, const std::vector<float> &volumes) = 0;
	virtual void setDeviceMute(uint32_t id, bool mute) = 0;
	virtual void setStreamVolume(uint32_t id, const std::vector<float> &volumes) = 0;
	virtual void setStreamMute(uint32_t id, bool mute) = 0;
	virtual void setDefaultDevice(uint32_t id) = 0;
	virtual void moveStream(uint32_t streamId, uint32_t deviceId) = 0;
	virtual void setDevicePort(uint32_t id, const std::string &port) = 0;
	virtual void setCardProfile(uint32_t id, const std::string &profile) = 0;

	/* convenience: one level for all channels, keeping the balance */
	void setDeviceLevel(uint32_t id, float level);
	void setStreamLevel(uint32_t id, float level);
	/** changes the default output by @p delta (e.g. +0.05), clamped to 0..max */
	void changeDefaultVolume(Audio::DeviceKind kind, float delta, float max);
	void toggleDefaultMute(Audio::DeviceKind kind);

	/** the model changed (FLTK thread) */
	std::function<void()> on_changed;
	/** connected to or disconnected from the sound server (FLTK thread) */
	std::function<void(bool)> on_connection;

protected:
	/* the backend's own lock; m_raw and m_rawPeaks are guarded by it */
	virtual void lock() = 0;
	virtual void unlock() = 0;
	/** called in the GUI thread after each model update, with the lock held */
	virtual void afterSync() {}

	/** call with the lock held, from any thread: copies m_raw to the GUI */
	void scheduleSync();

	/** copies the raw model to the GUI model (FLTK thread) */
	void syncToGui();
	static void awakeSync(void *data);

	Audio::Model m_raw;
	std::map<uint32_t, float> m_rawPeaks;
	bool m_peaksEnabled;

private:
	Audio::Model m_model;
	bool m_syncPending;
};

#endif
