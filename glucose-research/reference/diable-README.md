<p align="center"><img src="./DiaBLE/Assets.xcassets/AppIcon.appiconset/Icon.png" width="25%" /> &nbsp; &nbsp; <img src="https://github.com/gui-dos/DiaBLE/assets/7220550/901ad341-edfb-426e-9617-6763cf377447" width="20%"/> &nbsp; &nbsp; <img src="https://github.com/gui-dos/DiaBLE/assets/7220550/4d84f138-5b31-4db3-a407-f85265a78e66" width="20%"/></p>
<br><br>

**ChangeLog:**

* 03/07/2026 - 0.0.2 -  [Build 217](https://github.com/gui-dos/DiaBLE/commit/823e455)
  - **Libre 3 Direct-To-Watch** leveraging [Messina](https://github.com/awowogei/Messina/) one-shot server:<br><br><div align="center"><img src="https://github.com/user-attachments/assets/26986471-de74-4303-a9e4-fd0a9db932db" width="25%"/></div><br><br>

## Builds

To build the project, you have to duplicate the file _DiaBLE.xcconfig_, rename the copy to ***DiaBLEOverride.xcconfig*** (the missing reference displayed by Xcode in red should then point to it) and edit it by commenting out the trailing lines after `// Comment out the following...` and replacing `##TEAM_ID##` with your Apple Team ID, so that the first line should read, for example, `DEVELOPMENT_TEAM = Z25SC9UDC8`.

The NFC capabilities require a paid Apple Developer Program **annual membership**. If you won't [request](https://developer.apple.com/contact/request/notifications-critical-alerts-entitlement/) a *Critical Alert Notifications Entitlement*, you have to edit out the lines `<key>com.apple.developer.usernotifications.critical-alerts</key>` `<true/>` from the _.entitlements_ files in the folders _DiaBLE_ and _DiaBLE Watch_.

A public beta of DiaBLE is availaBLE on **[TestFlight](https://testflight.apple.com/join/s4vTFYpC)**: I'll periodically expunge anonymous users I didn't invite or who didn't sponsor me through [PayPal](https://paypal.me/guisor) $-). If you own an iPad you can download the [zipped archive](https://github.com/gui-dos/DiaBLE/archive/refs/heads/main.zip) of this repository and just tap _DiaBLE Playground.swiftpm_ to test the corresponding features.

Currently I am targeting only the latest betas of Xcode and iOS and focusing on the new **Libre 3** and **Dexcom G7**. Please consider my personal project still just a **prototype** (I am not even managing correcty the execution in the background yet), even though most open-source apps (and even commercial ones) make use of my naive NFC and BLE classes which unveil technical details not found elsewhere... ;-)

## Warnings

  * the temperature-based calibration algorithm has been derived from the old LibreLink 2.3: it is known that the Vendor improves its algorithms at every new release, smoothing the historical values and projecting the trend ones into the future to compensate the interstitial delay but these further stages aren't understood yet; I never was convinced by the simple linear regression models that others apply on finger pricks;
  * activating the BLE streaming of data on a Libre 2 will break other apps' pairings and you will have to reinstall them to get their alarms back again; in Test mode it is possiBLE however to eavesdrop on the incoming data of multiple apps running side-by-side by just activating the notifications on the known BLE characteristics: the same technique is used to analyze the Libre 3 incoming traffic since the Core Bluetooth connections are reference-counted;
  * connecting directly to a Libre 2/3 and a Dexcom G7 from an Apple Watch is currently just a proof of concept that it is technically possiBLE: keeping the connection in the background will require additional work and AFAIK nobody else is capaBLE of doing the job... :-P

## TODOs

* Libre 3++:
  - analyze "Libre by Abbott"'s Go runtime definitions and the extended ketone/lactate multi-analyte protocols
  - focus on our still selling points, the standalone Apple Watch app and sharing the data through HealthKit
* Dexcom G7:
  - J-PAKE authentication protocol (see [xDrip+'s _keks_](https://github.com/NightscoutFoundation/xDrip/blob/master/libkeks/))
* migrate to Swift 6 concurrency
* scrollable graph, offline trend arrow, landscape mode
* smooth the historic values and project the trend ones (see [LibreTransmitter](https://github.com/dabear/LibreTransmitter/commit/49b50d7995955b76861440e5e34a0accd064d18f))
* log: limit to a number of readings, prepend time, Share menu, record to a file
* new iOS Widgets and App Intents (see [OpenGlück](https://github.com/open-gluck))
* SwiftData and/or TabularData as persistence layers (see [Glupreview](https://github.com/solanovisitor/glupreview) for CoreML use)

## Contributions

Please submit your PRs on the _dev_ branch. DiaBLE's conceptual model is quite straightforward: `main` is the application _MainDelegate_ while `app` is the observable environmental _AppState_ viewmodel; they retain each other to allow simple expressions globally, such as `main.log()` and `app.device` to access the properties of the connected BLE peripheral or the current `app.sensor`.

---
###### ***Disclaimer: the decrypting keys I am publishing are not related to user accounts and can be dumped from the sensor memory by using DiaBLE itself. The online servers I am using probably are tracking your personal data but all the traffic sent/received by DiaBLE is clearly shown in its logs. The reverse-engineered code I am copying&pasting has been retrieved from other GitHub repos or reproduced simply by using open-source tools like Ghidra and `jadx-gui`.***
