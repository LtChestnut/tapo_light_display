// Copy this file to src/secrets.h (git-ignored) and fill in your details.
#pragma once

// 2.4 GHz Wi-Fi (the ESP32-S3 has no 5 GHz radio).
#define WIFI_SSID "your-ssid"
#define WIFI_PASSWORD "your-wifi-password"

// Tapo app account. Only used locally to derive the KLAP auth hash; never sent anywhere.
// Also enable Tapo app -> Me -> Third-Party Services -> Third-Party Compatibility.
#define TAPO_EMAIL "you@example.com"
#define TAPO_PASSWORD "your-tapo-password"

// Optional: the knob's own fixed address. Reserve it for the knob on the router, then set
// these to skip DHCP on every wake from deep sleep. Remove them to use plain DHCP.
// #define KNOB_IP "192.168.1.50"
// #define KNOB_GATEWAY "192.168.1.1"
// #define KNOB_SUBNET "255.255.255.0"
// #define KNOB_DNS "192.168.1.1"

// Bulb IPs. Give each a DHCP reservation on your router so these don't change.
// Find them with `kasa discover` (python-kasa) or your router's client list.
#define TAPO_BULB_IPS {"192.168.1.10", "192.168.1.11", "192.168.1.12"}
