#include <wait.h>
struct netfront_dev;
struct netfront_dev *init_netfront(char *nodename, void (*netif_rx)(unsigned char *data, int len), unsigned char rawmac[6]);
void netfront_xmit(struct netfront_dev *dev, unsigned char* data,int len);
void shutdown_netfront(struct netfront_dev *dev);

extern struct wait_queue_head netfront_queue;
