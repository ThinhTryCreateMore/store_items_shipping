#pragma once
#include <Arduino.h>
#include "esp_camera.h"
#include "mbedtls/sha256.h"

struct Result { String status = "error"; String name; int locker = 0; };