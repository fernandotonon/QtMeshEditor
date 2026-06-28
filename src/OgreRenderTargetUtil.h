#pragma once

#include <OgreRenderSystem.h>
#include <OgreRenderTarget.h>
#include <OgreRenderWindow.h>
#include <OgreRoot.h>

#include <string>

#include "Manager.h"

namespace OgreRenderTargetUtil {

/// Manual RTTs default to auto-update in Ogre, so every `renderOneFrame()`
/// re-renders them and GL3+ logs `GL_INVALID_OPERATION` from `glDrawBuffer(GL_BACK)`.
inline void configureOffscreenRenderTarget(Ogre::RenderTarget* target)
{
    if (target)
        target->setAutoUpdated(false);
}

/// After an offscreen RTT pass, re-bind the editor viewport so the next
/// `renderOneFrame()` does not inherit a stale FBO binding.
inline void restoreEditorRenderTarget()
{
    auto* root = Ogre::Root::getSingletonPtr();
    if (!root)
        return;
    auto* rs = root->getRenderSystem();
    if (!rs)
        return;

    Ogre::RenderWindow* editorWindow = nullptr;
    if (auto* mgr = Manager::getSingletonPtr()) {
        if (auto* rt = mgr->getRoot()->getRenderTarget("Viewport 0"))
            editorWindow = dynamic_cast<Ogre::RenderWindow*>(rt);
    }
    if (!editorWindow) {
        for (unsigned short i = 0; i < 16; ++i) {
            const std::string name = "Viewport " + std::to_string(i);
            if (auto* rt = root->getRenderTarget(name))
                if (auto* rw = dynamic_cast<Ogre::RenderWindow*>(rt)) {
                    editorWindow = rw;
                    break;
                }
        }
    }
    if (editorWindow)
        rs->_setRenderTarget(editorWindow);
}

} // namespace OgreRenderTargetUtil
