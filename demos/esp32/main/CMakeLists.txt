idf_component_register(
  SRCS "wake_demo.c"
  INCLUDE_DIRS ""
  REQUIRES esp_driver_i2s esp_timer heap wake
  EMBED_FILES "model.pwake"
  # Text embed adds the NUL terminator the engine's JSON parser needs.
  EMBED_TXTFILES "keywords.json")
