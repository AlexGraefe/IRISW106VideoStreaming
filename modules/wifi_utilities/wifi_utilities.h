#ifndef WIFI_H
#define WIFI_H

int my_wifi_init(void); // rename, currently wifi_init has a name clash with nxp library
int wifi_connect(char *ssid, char *psk);
int wifi_wait_for_ip_addr(char *ip_addr);
int wifi_disconnect(void);

#endif /* WIFI_H */