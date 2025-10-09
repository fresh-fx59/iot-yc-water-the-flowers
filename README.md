# Description

This code manages ESP32 device. It responsible for watering the flowers. The system consist of 6 valves, 6 rain sensors and 1 water pump.

Algorithm to water the flowers is as follows:
* rain sensor on
* get data if rain sensor is wet
* if rain sensor is wet, end algorithm
* if rain sensor is dry, then open valve
* turn on pump and monitor rain sensor
* if rain sensor wet, then close the valve and turn off the pump
* end algorithm

This algorithm is used separately for each of 6 valves. State produces to MQTT topic on each state changes. Errors in producing messages to MQTT topics don't affect the algorithm itself.
