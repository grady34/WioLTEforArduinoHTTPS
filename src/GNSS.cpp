bool GNSS::open_GNSS(void) {
    int errCounts = 0;

    //Open GNSS funtion
    while (!check_with_cmd("AT+QGPS?\r\n", "+QGPS: 1", CMD, 2, 2000, UART_DEBUG)) {
        errCounts ++;
        if (errCounts > 5) {
            return false;
        }
        check_with_cmd("AT+QGPS=1\r\n", "OK", CMD, 2, 2000, UART_DEBUG);
        delay(1000);
    }

    return true;
}

bool GNSS::getCoordinate(void) {
    int tmp = 0;
    char* p = NULL;
    uint8_t str_len = 0;
    char buffer[128];

    clean_buffer(buffer, 128);
    send_cmd("AT+QGPSLOC?\r\n");
    read_buffer(buffer, 128, 2);
    // SerialUSB.println(buffer);
    if (NULL != (p = strstr(buffer, "+CME ERROR"))) {
        return false;
    }

    // +QGPSLOC: 084757.700,2235.0272N,11357.9730E,1.6,40.0,3,171.43,0.0,0.0,290617,10
    else if (NULL != (p = strstr(buffer, "+QGPSLOC:"))) {
        p += 10;
        p = strtok(buffer, ","); // time
        p = strtok(NULL, ",");  // latitude
        sprintf(str_latitude, "%s", p);
        latitude = strtod(p, NULL);
        tmp = (int)(latitude / 100);
        latitude = (double)(tmp + (latitude - tmp * 100) / 60.0);

        // Get North and South status
        str_len = strlen(p);
        if ((*(p + str_len - 1) != 'N') && (*(p + str_len - 1) != 'S')) {
            North_or_South[0] = '0';
            North_or_South[1] = '\0';
        } else {
            North_or_South[0] = *(p + str_len - 1);
            North_or_South[1] = '\0';
        }

        p = strtok(NULL, ",");  // longitude
        sprintf(str_longitude, "%s", p);
        longitude = strtod(p, NULL);

        // Get West and East status
        str_len = strlen(p);
        if ((*(p + str_len - 1) != 'W') && (*(p + str_len - 1) != 'E')) {
            West_or_East[0] = '0';
            West_or_East[1] = '\0';
        } else {
            West_or_East[0] = *(p + str_len - 1);
            West_or_East[1] = '\0';
        }

        tmp = (int)(longitude / 100);
        longitude = (double)(tmp + (longitude - tmp * 100) / 60.0);

        // if(North_or_South[0] == 'S'){
        //     // latitude = 0.0 - latitude;
        // } else if(North_or_South[0] = 'N'){
        //     latitude = 0.0 - latitude;
        // }

        // if(West_or_East[0] == 'W'){
        //     // longitude = 0.0 - longitude;
        // } else if(West_or_East[0] = 'E'){
        //     longitude = 0.0 - longitude;
        // }

        doubleToString(longitude, latitude);
    } else {
        return false;
    }
    return true;
}

void GNSS::doubleToString(double longitude, double latitude) {
    int u8_lon = (int)longitude;
    int u8_lat = (int)latitude;
    uint32_t u32_lon = (longitude - u8_lon) * 1000000;
    uint32_t u32_lat = (latitude - u8_lat) * 1000000;

    sprintf(str_longitude, "%d.%lu", u8_lon, u32_lon);
    sprintf(str_latitude, "%d.%lu", u8_lat, u32_lat);
}
