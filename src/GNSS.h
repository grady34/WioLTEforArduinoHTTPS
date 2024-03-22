#ifndef __GNSS_H__
#define __GNSS_H__

#include "WioLTE.h"
#include "UART_Interface.h"
#include "stdio.h"

typedef enum NMEA_TYPE {
    GGA = 0,
    RMC,
    GSV,
    GSA,
    VTG,
    GNS
} NMEA_type;


class GNSS : public WioLTE {
  public:
    double longitude;
    double latitude;
    char str_longitude[16];
    char str_latitude[16];
    double ref_longitude = 22.584322;
    double ref_latitude = 113.966678;
    char North_or_South[2];
    char West_or_East[2];


    bool open_GNSS(int mode);
    bool close_GNSS(void);

    /**
        open GNSS
    */
    bool open_GNSS(void);

    /**
        Convert double coordinate data to string
    */
    void doubleToString(double longitude, double latitude);


    /** Get coordinate infomation

    */
    bool getCoordinate(void);

    /**
        Aquire GPS sentence
    */
    
};

#endif
