#ifndef __CONFIG_H_
#define __CONFIG_H_

#define MAX_IPADDR_STRLEN      15
#define MAX_SERVER_NAME_LEN    10
/*---------------------------------------------------------------------------*/
struct proxy_context*
LoadConfigData(const char *fname);
/*---------------------------------------------------------------------------*/
#endif /* __CONFIG_H_ */
