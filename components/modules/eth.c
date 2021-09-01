// When this isn't enabled, the esp_eth.h header isn't available
#ifdef CONFIG_ETH_ENABLED
#include <string.h>

#include "module.h"
#include "lauxlib.h"
#include "lextra.h"
#include "lmem.h"
#include "nodemcu_esp_event.h"
#include "ip_fmt.h"
#include "common.h"
#include "esp_netif.h"
#include "esp_eth.h"
#include "esp_eth_phy.h"
#include "driver/gpio.h"


typedef esp_eth_phy_t *(*new_eth_phy_fn)(const eth_phy_config_t *config);

typedef enum {
  ETH_PHY_DP83848,
  ETH_PHY_IP101,
  ETH_PHY_KSZ8041,
  ETH_PHY_KSZ8081,
  ETH_PHY_LAN8720,
  ETH_PHY_RTL8201,
  ETH_PHY_MAX
} eth_phy_t;

static const new_eth_phy_fn new_eth_phy[] = {
  [ETH_PHY_DP83848] = esp_eth_phy_new_dp83848,
  [ETH_PHY_IP101]   = esp_eth_phy_new_ip101,
  [ETH_PHY_KSZ8041] = esp_eth_phy_new_ksz8041,
  [ETH_PHY_KSZ8081] = esp_eth_phy_new_ksz8081,
  [ETH_PHY_LAN8720] = esp_eth_phy_new_lan8720,
  [ETH_PHY_RTL8201] = esp_eth_phy_new_rtl8201,
};
_Static_assert(sizeof(new_eth_phy) == (sizeof(new_eth_phy[0]) * ETH_PHY_MAX),
  "missing phy create func");

static esp_netif_t *eth = NULL;
esp_eth_handle_t eth_handle = NULL;


// --- Event handling -----------------------------------------------------

typedef void (*fill_cb_arg_fn) (lua_State *L, const void *data);
typedef struct
{
  const char *name;
  esp_event_base_t *event_base_ptr;
  int32_t event_id;
  fill_cb_arg_fn fill_cb_arg;
} event_desc_t;

#define ARRAY_LEN(a) (sizeof(a) / sizeof(a[0]))

static void eth_got_ip (lua_State *L, const void *data);
static void empty_arg (lua_State *L, const void *data) {}

static const event_desc_t events[] =
{
  { "start",        &ETH_EVENT, ETHERNET_EVENT_START,        empty_arg   },
  { "stop",         &ETH_EVENT, ETHERNET_EVENT_STOP,         empty_arg   },
  { "connected",    &ETH_EVENT, ETHERNET_EVENT_CONNECTED,    empty_arg   },
  { "disconnected", &ETH_EVENT, ETHERNET_EVENT_DISCONNECTED, empty_arg   },
  { "got_ip",       &IP_EVENT,  IP_EVENT_ETH_GOT_IP,         eth_got_ip  },
};

#define ARRAY_LEN(a) (sizeof(a) / sizeof(a[0]))
static int event_cb[ARRAY_LEN(events)];

static int eth_event_idx_by_id( esp_event_base_t base, int32_t id )
{
  for (unsigned i = 0; i < ARRAY_LEN(events); ++i)
    if (*events[i].event_base_ptr == base && events[i].event_id == id)
      return i;
  return -1;
}

static int eth_event_idx_by_name( const char *name )
{
  for (unsigned i = 0; i < ARRAY_LEN(events); ++i)
    if (strcmp( events[i].name, name ) == 0)
      return i;
  return -1;
}


static void eth_got_ip( lua_State *L, const void *data )
{
  const ip_event_got_ip_t *got_ip_info =
    (const ip_event_got_ip_t *)data;
  const esp_netif_ip_info_t *ip_info = &got_ip_info->ip_info;

  // on_event() has prepared a table on top of stack. fill it with cb-specific fields:
  //   ip, netmask, gw
  char ipstr[IP_STR_SZ] = { 0 };
  ip4str_esp( ipstr, &ip_info->ip );
  lua_pushstring( L, ipstr );
  lua_setfield( L, -2, "ip" );

  ip4str_esp( ipstr, &ip_info->netmask );
  lua_pushstring( L, ipstr );
  lua_setfield( L, -2, "netmask" );

  ip4str_esp(ipstr, &ip_info->gw );
  lua_pushstring( L, ipstr );
  lua_setfield( L, -2, "gw" );
}

static void on_event(esp_event_base_t base, int32_t id, const void *data)
{
  int idx = eth_event_idx_by_id( base, id );
  if (idx < 0 || event_cb[idx] == LUA_NOREF)
    return;

  lua_State *L = lua_getstate();
  int top = lua_gettop( L );
  lua_rawgeti( L, LUA_REGISTRYINDEX, event_cb[idx] );
  lua_pushstring( L, events[idx].name );
  lua_createtable( L, 0, 5 );
  events[idx].fill_cb_arg( L, data );
  luaL_pcallx( L, 2, 0 );

  lua_settop( L, top );
}

NODEMCU_ESP_EVENT(ETH_EVENT, ETHERNET_EVENT_START,           on_event);
NODEMCU_ESP_EVENT(ETH_EVENT, ETHERNET_EVENT_STOP,            on_event);
NODEMCU_ESP_EVENT(ETH_EVENT, ETHERNET_EVENT_CONNECTED,       on_event);
NODEMCU_ESP_EVENT(ETH_EVENT, ETHERNET_EVENT_DISCONNECTED,    on_event);
NODEMCU_ESP_EVENT(IP_EVENT,  IP_EVENT_ETH_GOT_IP,            on_event);


// Lua API

static int leth_set_mac( lua_State *L )
{
  uint8_t mac[6];
  const char *macstr = luaL_checkstring( L, 1 );

  if (6 != sscanf( macstr, "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
                   &mac[0], &mac[1], &mac[2], &mac[3], &mac[4], &mac[5] )) {
    return luaL_error( L, "invalid mac string" );
  }

  if (ESP_OK != esp_netif_set_mac( eth, mac )) {
    return luaL_error( L, "error setting mac" );
  }

  return 0;
}

static int leth_get_mac( lua_State *L )
{
  char temp[64];
  uint8_t mac[6] = { 0, };

  esp_netif_get_mac( eth, mac );
  snprintf( temp, 63, MACSTR, MAC2STR(mac) );
  lua_pushstring( L, temp );
  return 1;
}

static int leth_get_speed( lua_State *L )
{
  eth_speed_t speed;
  if (esp_eth_ioctl(eth_handle, ETH_CMD_G_SPEED, &speed) != ESP_OK)
    return luaL_error(L, "failed to get eth speed");

  switch (speed) {
  case ETH_SPEED_10M:
    lua_pushnumber( L, 10 );
    break;
  case ETH_SPEED_100M:
    lua_pushnumber( L, 100 );
    break;
  default:
    return luaL_error( L, "invalid speed" );
  }
  return 1;
}

static int leth_on( lua_State *L )
{
  const char *event_name = luaL_checkstring( L, 1 );
  if (!lua_isnoneornil( L, 2 )) {
    luaL_checkfunction( L, 2 );
  }
  lua_settop( L, 2 );

  int idx = eth_event_idx_by_name( event_name );
  if (idx < 0)
    return luaL_error( L, "unknown event: %s", event_name );

  luaL_unref( L, LUA_REGISTRYINDEX, event_cb[idx] );
  event_cb[idx] = lua_isnoneornil( L, 2 ) ?
    LUA_NOREF : luaL_ref( L, LUA_REGISTRYINDEX );

  return 0;
}

static int leth_init( lua_State *L )
{
  if (eth != NULL)
    return luaL_error(L, "ethernet interface already set up");

  int stack = 0;
  int top = lua_gettop( L );

  luaL_checktype( L, ++stack, LUA_TTABLE );
  // temporarily copy option table to top of stack for opt_ functions
  lua_pushvalue( L, stack );

  eth_mac_config_t mac_cfg = ETH_MAC_DEFAULT_CONFIG();

  mac_cfg.smi_mdc_gpio_num =
    opt_checkint_range( L, "mdc",   -1, GPIO_NUM_0, GPIO_NUM_MAX-1 );
  mac_cfg.smi_mdio_gpio_num =
    opt_checkint_range( L, "mdio",  -1, GPIO_NUM_0, GPIO_NUM_MAX-1 );

  eth_phy_config_t phy_cfg = ETH_PHY_DEFAULT_CONFIG();

  phy_cfg.phy_addr = opt_checkint_range( L, "addr", -1, -1, 31 );
  phy_cfg.reset_gpio_num =
    opt_checkint_range( L, "power", -1, -1, GPIO_NUM_MAX-1 ); // optional

  eth_phy_t phy_type = opt_checkint_range( L, "phy", -1, 0, ETH_PHY_MAX );

  lua_settop( L, top );

  esp_eth_mac_t *mac = esp_eth_mac_new_esp32(&mac_cfg);
  esp_eth_phy_t *phy = new_eth_phy[phy_type](&phy_cfg);

  esp_eth_config_t eth_cfg = ETH_DEFAULT_CONFIG(mac, phy);

  esp_err_t err = esp_eth_driver_install(&eth_cfg, &eth_handle);
  if (err != ESP_OK)
    goto cleanup_mac_phy;

  esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_ETH();
  esp_netif_t *new_eth = esp_netif_new(&netif_cfg);

  err = esp_eth_set_default_handlers(new_eth);
  if (err != ESP_OK)
    goto cleanup_netif;

  void *glue = esp_eth_new_netif_glue(eth_handle);
  err = esp_netif_attach(new_eth, glue);
  if (err != ESP_OK)
    goto cleanup_glue;

  err = esp_eth_start(eth_handle);
  if (err != ESP_OK)
    goto cleanup_glue;

  eth = new_eth;
  return 0;

cleanup_glue:
  esp_eth_del_netif_glue(glue);

cleanup_netif:
  esp_netif_destroy(new_eth);

cleanup_mac_phy:
  if (mac)
    mac->del(mac);
  if (phy)
    phy->del(phy);

  eth_handle = NULL;

  return luaL_error(L, "failed to init ethernet, code %d", err);
}


LROT_BEGIN(eth, NULL, 0)
  LROT_FUNCENTRY( init,       leth_init )
  LROT_FUNCENTRY( on,         leth_on )
  LROT_FUNCENTRY( get_speed,  leth_get_speed )
  LROT_FUNCENTRY( get_mac,    leth_get_mac )
  LROT_FUNCENTRY( set_mac,    leth_set_mac )

  LROT_NUMENTRY( PHY_DP83848, ETH_PHY_DP83848 )
  LROT_NUMENTRY( PHY_IP101,   ETH_PHY_IP101 )
  LROT_NUMENTRY( PHY_KSZ8041, ETH_PHY_KSZ8041 )
  LROT_NUMENTRY( PHY_KSZ8081, ETH_PHY_KSZ8081 )
  LROT_NUMENTRY( PHY_LAN8720, ETH_PHY_LAN8720 )
  LROT_NUMENTRY( PHY_RTL8201, ETH_PHY_RTL8201 )
LROT_END(eth, NULL, 0)

NODEMCU_MODULE(ETH, "eth", eth, NULL);
#endif