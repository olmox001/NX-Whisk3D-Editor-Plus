#ifndef W3D_LLM_WORKSPACE_H
#define W3D_LLM_WORKSPACE_H

#ifdef W3D_MCP

#include <string>
#include <vector>

struct W3dLlmWorkspaceResult {
    bool ok;
    std::string text;
    W3dLlmWorkspaceResult(bool success = true, const std::string& message = std::string())
        : ok(success), text(message) {}
};

void W3dLlmWorkspaceInit();
bool W3dLlmWorkspaceReady();
std::string W3dLlmWorkspaceContext(bool verbose);

W3dLlmWorkspaceResult W3dLlmSessionOpen(const std::string& title,
                                         const std::string& objective);
W3dLlmWorkspaceResult W3dLlmSessionAddPlan(const std::vector<std::string>& points);
W3dLlmWorkspaceResult W3dLlmSessionSwitch(const std::string& sessionId);
W3dLlmWorkspaceResult W3dLlmSessionClose(const std::string& result,
                                          const std::string& comment);

W3dLlmWorkspaceResult W3dLlmTaskOpen(const std::string& title,
                                      const std::string& planPointId);
W3dLlmWorkspaceResult W3dLlmTaskSwitch(const std::string& taskId);
W3dLlmWorkspaceResult W3dLlmTaskSuspend(const std::string& comment);
W3dLlmWorkspaceResult W3dLlmTaskResume(const std::string& taskId);
W3dLlmWorkspaceResult W3dLlmTaskCommit(const std::string& summary,
                                        const std::string& comment);
W3dLlmWorkspaceResult W3dLlmTaskClose(const std::string& outcome,
                                       const std::string& comment);


bool W3dLlmWorkspaceCanMutate(std::string& reason);
void W3dLlmWorkspaceRecordChange(const std::string& tool,
                                  const std::string& detail);
W3dLlmWorkspaceResult W3dLlmWorkspaceRecordTest(const std::string& commands,
                                                 const std::string& expected,
                                                 const std::string& output,
                                                 bool passed);

#endif
#endif
