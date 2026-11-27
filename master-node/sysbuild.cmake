# SPDX-License-Identifier: Apache-2.0
#
# Adds Zephyr's hci_ipc sample (Bluetooth controller) as the network core image.

if(SB_CONFIG_NET_CORE_IMAGE_HCI_IPC)
  set(NET_APP hci_ipc)
  set(NET_APP_SRC_DIR ${ZEPHYR_BASE}/samples/bluetooth/${NET_APP})

  ExternalZephyrProject_Add(
    APPLICATION ${NET_APP}
    SOURCE_DIR ${NET_APP_SRC_DIR}
    BOARD ${SB_CONFIG_NET_CORE_BOARD}
  )

  sysbuild_add_dependencies(CONFIGURE ${DEFAULT_IMAGE} ${NET_APP})
  sysbuild_add_dependencies(FLASH ${DEFAULT_IMAGE} ${NET_APP})
endif()
