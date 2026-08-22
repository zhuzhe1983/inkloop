idf_component_register(
  SRCS "inkloop_cloud_client.cpp"
  INCLUDE_DIRS "include"
  REQUIRES
    espressif__cjson
    inkloop_myai
    inkloop_storage
)
