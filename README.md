# IoT Valve Controller

## Why I Made This

I built this project as my capstone for studying IoT systems and cloud integration. I wanted to learn how to connect hardware to the cloud and control it remotely from anywhere. This project taught me about embedded systems, cloud databases, web development, and security in one complete system.

## Goal

The goal is to create a system that lets users control a water valve remotely using their phone or computer. Each ESP32 device gets a unique ID and connects to Firebase, allowing authorized users to open or close the valve in real-time. The system is secure because only authenticated users can control their own devices.

## How It Works

The ESP32 generates a unique device ID on first boot and displays it on a setup portal. Users access a web application, sign in with Google, add their device using the device ID, and can then open or close the valve remotely. The system uses Firebase as the bridge between the web app and the ESP32 device. When a user clicks open or close, the command goes to Firebase, the ESP32 reads it, and controls the physical valve. The valve status returns to Firebase and updates the web app in real-time. Firebase security rules ensure that users can only control devices they own.

