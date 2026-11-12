#ifndef W3D_LLM_HISTORY_H
#define W3D_LLM_HISTORY_H

// ============================================================================
//  W3dLlmHistory registra el flujo real de interaccion del editor y los cambios
//  de estado resultantes. Sirve para auditoria LLM y para reproducir flujos
//  humanos.
//
//  Se habilita con W3D_LLM_HISTORY=<path>. W3D_HISTORIAL queda como nombre de
//  compatibilidad. El archivo se vacia tras cada linea para que un fallo no
//  descarte la traza ya completada.
//
//  El grabador captura:
//    - entrada: teclas, clicks, elecciones de menu, arrastres y rueda;
//    - estado resultante: modo, seleccion, transformaciones, cursor,
//      modificadores y cambios de la pila de undo.
// ============================================================================
#ifdef W3D_MCP

#include "app/controles.h"   // SDL_Event

void W3dLlmHistoryIniciar();
bool W3dLlmHistoryActivo();
void W3dLlmHistoryEvento(const SDL_Event& e);      // controles.cpp, ANTES de procesar el evento (lo que hay debajo)
void W3dLlmHistoryCuadro();                        // main.cpp, cada cuadro: anota lo que cambio
void W3dLlmHistoryUndo(const char* tipo);          // Undo.cpp: un paso nuevo en el Ctrl+Z (el tipo de comando)

#endif
#endif
