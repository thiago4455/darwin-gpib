//
//  ExtensionManager.swift
//  darwin-gpib
//
//  Created by Thiago Mattos on 14/07/25.
//


import Foundation
import SystemExtensions
import os.log

class ExtensionManager : NSObject, OSSystemExtensionRequestDelegate {
    
    static let shared = ExtensionManager()
     
    func activate() {
        os_log("sysex activation request for %@", "app.saturno.darwin-gpib.driver")
        let activationRequest = OSSystemExtensionRequest.activationRequest(forExtensionWithIdentifier: "app.saturno.darwin-gpib.driver", queue: .main)
        activationRequest.delegate = self
        OSSystemExtensionManager.shared.submitRequest(activationRequest)
    }
    
    func deactivate() {
        // This doesn't seem to work in b1 not sure why
        let activationRequest = OSSystemExtensionRequest.deactivationRequest(forExtensionWithIdentifier: "app.saturno.darwin-gpib.driver", queue: .main)
        activationRequest.delegate = self
        OSSystemExtensionManager.shared.submitRequest(activationRequest)
    }
    
    func request(_ request: OSSystemExtensionRequest, actionForReplacingExtension existing: OSSystemExtensionProperties, withExtension ext: OSSystemExtensionProperties) -> OSSystemExtensionRequest.ReplacementAction {
        os_log("sysex actionForReplacingExtension %@ %@", existing, ext)
        
        return .replace
    }
    
    func requestNeedsUserApproval(_ request: OSSystemExtensionRequest) {
        os_log("sysex needsUserApproval")
        
    }
    
    func request(_ request: OSSystemExtensionRequest, didFinishWithResult result: OSSystemExtensionRequest.Result) {
        os_log("sysex didFinishWithResult %@", result.rawValue)
        
    }
    
    func request(_ request: OSSystemExtensionRequest, didFailWithError error: Error) {
        os_log("sysex didFailWithError %@", error.localizedDescription)
    }
}
