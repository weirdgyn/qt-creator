// Copyright (C) 2023 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0

#pragma once

#include "copilotclient.h"

#include <extensionsystem/iplugin.h>

namespace TextEditor { class TextEditorWidget; }

namespace Copilot {
namespace Internal {

class CopilotPlugin : public ExtensionSystem::IPlugin
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "org.qt-project.Qt.QtCreatorPlugin" FILE "Copilot.json")

public:
    ~CopilotPlugin();

    void initialize() override;
    void extensionsInitialized() override;

private:
    CopilotClient *m_client{nullptr};
};

} // namespace Internal
} // namespace Copilot
