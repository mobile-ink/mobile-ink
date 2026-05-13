package com.mathnotes.mobileink

import com.facebook.react.TurboReactPackage
import com.facebook.react.bridge.NativeModule
import com.facebook.react.bridge.ReactApplicationContext
import com.facebook.react.module.annotations.ReactModuleList
import com.facebook.react.module.model.ReactModuleInfo
import com.facebook.react.module.model.ReactModuleInfoProvider
import com.facebook.react.uimanager.ViewManager

/**
 * TurboReactPackage for the native drawing module.
 *
 * Using TurboReactPackage instead of ReactPackage is critical for React Native 0.81+
 * with new architecture enabled. It provides module metadata via getReactModuleInfoProvider()
 * which allows RN to register modules without loading native libraries during startup scan.
 */
@ReactModuleList(nativeModules = [MobileInkModule::class])
class MobileInkPackage : TurboReactPackage() {

    override fun createNativeModules(reactContext: ReactApplicationContext): List<NativeModule> {
        return listOf(MobileInkModule(reactContext))
    }

    override fun getModule(name: String, reactContext: ReactApplicationContext): NativeModule? {
        return when (name) {
            MobileInkModule.NAME -> MobileInkModule(reactContext)
            else -> null
        }
    }

    override fun getReactModuleInfoProvider(): ReactModuleInfoProvider {
        return ReactModuleInfoProvider {
            mapOf(
                MobileInkModule.NAME to ReactModuleInfo(
                    MobileInkModule.NAME,
                    MobileInkModule::class.java.name,
                    false,  // canOverrideExistingModule
                    false,  // needsEagerInit
                    false,  // hasConstants - we have no getConstants()
                    false,  // isCxxModule - this is Kotlin, not C++
                    false   // isTurboModule - false to use legacy NativeModules bridge
                )
            )
        }
    }

    override fun createViewManagers(reactContext: ReactApplicationContext): List<ViewManager<*, *>> {
        return listOf(MobileInkCanvasViewManager(reactContext))
    }
}
