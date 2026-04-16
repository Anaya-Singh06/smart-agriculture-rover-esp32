# ESP32-Based Smart Agriculture Rover for Multi-Point Monitoring and Decision Support

## Overview
This project implements a mobile embedded system using ESP32 that collects environmental data (soil moisture, temperature, humidity) from multiple locations in a farm and provides intelligent suggestions for agricultural decision-making.

The system is designed to operate as an edge-based solution where all processing and decision-making occur on the ESP32, and a web dashboard is used for monitoring and control.

---

## Features
-  Rover-based multi-point data collection  
-  Soil moisture sensing  
-  Temperature and humidity monitoring  
-  Ultrasonic obstacle detection  
-  Web dashboard hosted on ESP32  
-  Real-time data updates using AJAX  
-  Edge-based decision logic  
-  Finite State Machine (FSM) based control  
-  Manual and semi-autonomous navigation  

---

## Decision Logic
The system generates intelligent suggestions based on sensor data:

- Soil moisture low → **Water Required**  
- High temperature → **Temperature Alert**  
- Low humidity → **Low Humidity Warning**  
- Obstacle detected → **Stop Rover**  

---

## Hardware Components
- ESP32 Microcontroller  
- Soil Moisture Sensor  
- DHT11 Temperature & Humidity Sensor  
- Ultrasonic Sensor (HC-SR04)  
- Servo Motor  
- TB6612FNG Motor Driver  
- DC Motors and Rover Chassis  

---

## System Architecture
The system consists of:

- **Sensors** → Collect environmental data  
- **ESP32** → Processes data and runs decision logic  
- **Motor Driver** → Controls rover movement  
- **Web Dashboard** → Displays real-time data  

(Refer to `diagrams/block_diagram.png`)

---

## Working Principle
1. Rover moves across the field  
2. Stops at specific points  
3. Collects sensor data  
4. Processes data locally on ESP32  
5. Displays results on web dashboard  
6. Provides smart agricultural suggestions  

---

## Web Interface
- Hosted on ESP32 (Access Point mode)  
- Accessible via browser (192.168.4.1)  
- Displays:
  - Soil moisture  
  - Temperature  
  - Humidity  
  - Distance  
  - Rover status  
  - Smart suggestions  

---

## Test Cases

| Condition        | Expected Output       |
|-----------------|---------------------|
| Dry soil        | Water needed        |
| High temperature| Alert message       |
| Normal condition| System OK           |
| Obstacle        | Rover stops         |

---

## Repository Structure

smart-agriculture-rover-esp32/
│
├── code/ # ESP32 source code
├── documents/ # Reports (Review 1, 2, 3)
├── diagrams/ # Block, FSM, circuit diagrams
├── components/ # Bill of materials
├── media/ # Demo images and video link
└── README.md

---

## Bill of Materials
Refer to:

components/bill_of_materials.md

---

## Communication
- WiFi (ESP32 Access Point Mode)  
- HTTP Protocol  
- AJAX for real-time updates  

---

## Performance Metrics
- Response time: < 1 second  
- Sensor update rate: ~1 Hz  
- Power consumption: Low to moderate  
- Communication reliability: High (local network)  

---

## Calibration & Validation
- Soil sensor calibrated using dry and wet soil values  
- Multiple readings averaged for accuracy  
- Threshold values experimentally determined  

---

## Future Work
- Automatic irrigation using relay-controlled water pumps  
- Pesticide and nutrient spraying system  
- AI/ML-based prediction models (e.g., LSTM)  
- Camera-based crop monitoring  
- Multi-rover coordination for large farms

