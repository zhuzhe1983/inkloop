idf_component_register(
  SRCS
    "AigcStreamDecoder.cpp"
    "CanonicalJsonCodec.cpp"
    "CredentialPersistence.cpp"
    "EndpointPolicy.cpp"
    "GatewayProbeContract.cpp"
    "MyAiClient.cpp"
    "WssIngress.cpp"
  INCLUDE_DIRS "include"
  PRIV_INCLUDE_DIRS "include/inkloop/myai"
)
