#pragma once

#include "lwip/netif.h"
#include "lwip/pbuf.h"

int wifi_manager_lwip_hook_ip4_input(struct pbuf *pbuf, struct netif *input_netif);
