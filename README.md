Project Title

ESP32-Based Smart Agriculture Rover for Multi-Point Monitoring and Decision Support

Overview

This project implements a mobile embedded system using ESP32 that collects environmental data (soil moisture, temperature, humidity) across multiple locations and provides intelligent agricultural suggestions.

Features
Rover-based multi-point sensing
Soil moisture, temperature, humidity monitoring
Ultrasonic obstacle detection
Web dashboard (real-time updates)
Finite State Machine-based control
Edge-based decision system
Decision Logic
Soil moisture low → Water required
High temperature → Alert
Low humidity → Warning
Obstacle → Stop rover
Hardware Used
ESP32
Soil Moisture Sensor
DHT11 Sensor
Ultrasonic Sensor
Servo Motor
TB6612FNG Motor Driver
Web Interface
Hosted on ESP32 (AP mode)
AJAX-based live updates
Mission planner
Architecture


Test Cases
Condition	Output
Dry soil	Water needed
High temp	Warning
Obstacle	Stop
Future Work
Automatic irrigation using water pump
Pesticide spraying system
Camera-based crop monitoring
AI-based prediction models
