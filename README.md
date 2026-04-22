# PD

Repo local cu o solutie MSVC pentru cele trei cerinte:

1. afisarea la iesirea standard a tuturor valorilor dintr-o subcheie de Registry;
2. listarea metaparametrilor unui device conectat la PC folosind apeluri Windows API.
3. crearea unui serviciu sistem care afiseaza mesajul `Hello World!` la initializare.

## Structura

- `PD.sln` - solutia Visual Studio
- `PDApp/PDApp.vcxproj` - proiectul C++ de tip consola
- `PDApp/main.cpp` - implementarea WinAPI

## Cerinte

- Visual Studio 2022 sau Build Tools cu toolset `v143`
- Windows SDK cu `SetupAPI`
- Workload-ul `Desktop development with C++` instalat
- Pentru cerinta 3, rularea comenzilor de instalare/pornire/oprire serviciu necesita PowerShell pornit cu drepturi de administrator

## Build

Din `Developer PowerShell for VS 2022` sau dintr-un PowerShell normal daca apelezi `MSBuild.exe` direct:

```powershell
& "C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" .\PD.sln /p:Configuration=Release /p:Platform=x64
```

Executabilul rezultat va fi in:

```text
bin\x64\Release\PDApp.exe
```

## Rulare

### 1. Listare valori Registry

Varianta cu radacina si subcheie separate:

```powershell
.\bin\x64\Release\PDApp.exe registry HKLM "SOFTWARE\Microsoft\Windows NT\CurrentVersion"
```

Varianta cu calea completa:

```powershell
.\bin\x64\Release\PDApp.exe registry "HKCU\Software\Microsoft\Windows\CurrentVersion\Explorer"
```

### 2. Listare device-uri conectate

Pentru a vedea device-urile prezente si indexul fiecaruia:

```powershell
.\bin\x64\Release\PDApp.exe devices
```

Pentru a lista metaparametrii pentru un device ales dupa index:

```powershell
.\bin\x64\Release\PDApp.exe device 0
```

Alternativ, poti selecta device-ul direct dupa `Instance ID`:

```powershell
.\bin\x64\Release\PDApp.exe device-id "USB\VID_XXXX&PID_YYYY\..."
```

### 3. Serviciu sistem Hello World

Instalarea serviciului:

```powershell
.\bin\x64\Release\PDApp.exe service install
```

Pornirea serviciului:

```powershell
.\bin\x64\Release\PDApp.exe service start
```

La initializare, serviciul trimite mesajul `Hello World!` catre sesiunea activa Windows folosind `WTSSendMessageW`. Mesajul este publicat si in Event Log/debug output ca fallback verificabil.

Verificarea starii:

```powershell
.\bin\x64\Release\PDApp.exe service status
```

Oprirea si dezinstalarea serviciului:

```powershell
.\bin\x64\Release\PDApp.exe service stop
.\bin\x64\Release\PDApp.exe service uninstall
```

## Detalii de implementare

- Functionalitatea Registry foloseste `RegOpenKeyExW`, `RegQueryInfoKeyW` si `RegEnumValueW`.
- Functionalitatea pentru device-uri foloseste `SetupDiGetClassDevsW`, `SetupDiEnumDeviceInfo`, `SetupDiGetDeviceInstanceIdW`, `SetupDiGetDevicePropertyKeys` si `SetupDiGetDevicePropertyW`.
- Pentru cerinta 2, aplicatia enumera proprietatile reale expuse de Windows pentru device-ul selectat si afiseaza atat cheia proprietatii, cat si valoarea ei.
- Functionalitatea de serviciu foloseste `CreateServiceW`, `StartServiceCtrlDispatcherW`, `RegisterServiceCtrlHandlerExW`, `SetServiceStatus`, `WTSSendMessageW` si `ReportEventW`.
