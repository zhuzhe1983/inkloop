idf_component_register(
  SRCS
    "task_topology.cpp"
    "cadence_policy.cpp"
    "runtime_admission.cpp"
    "runtime_telemetry.cpp"
    "runtime_supervisor.cpp"
  INCLUDE_DIRS "include"
  REQUIRES esp_timer freertos heap inkloop_contracts
)
