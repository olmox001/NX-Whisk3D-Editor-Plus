#ifndef W3D_LLM_CONTROL_H
#define W3D_LLM_CONTROL_H
// ============================================================================
//  Control LLM de Whisk3D: "whisk3d --llm" expone el editor por stdin/stdout.
//  "--mcp" conserva compatibilidad estricta con Model Context Protocol sobre el mismo transporte.
//  Solo en PC (W3D_MCP): ver W3dLlmControl.cpp.
// ============================================================================
#ifdef W3D_MCP
bool W3dLlmIniciar(bool terminalMode);    // al arrancar, antes que nada imprima: toma stdin/stdout para el protocolo
bool W3dLlmActivo();
bool W3dLlmAtender();    // en cada vuelta del loop: responde lo que llego. false = el cliente se fue (cerrar)
#endif

#endif
