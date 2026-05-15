# LED Runner pro Loxone

Tento projekt ovládá až 4 jednobarevné WS2811 LED pásky přes ESP8266 a jednoduché HTTP příkazy.

Aktuální stav firmware:

- rozsvěcení je animované po jednotlivých LED
- zhasnutí je vždy okamžité, bez zpětného chodu
- konfigurace se ukládá do EEPROM s validací přes verzi a checksum
- lze nastavit maximální jas v rozsahu 1 až 255
- lze přepnout mezi DHCP a pevnou IP adresou
- webové rozhraní umí základní test ON/OFF a uložení konfigurace

## Hardware

Pásky jsou v kódu mapované na tyto GPIO piny:

- pásek 1: GPIO0
- pásek 2: GPIO2
- pásek 3: GPIO4
- pásek 4: GPIO5

Praktické poznámky:

- WS2811 pásek napájej ze samostatného zdroje, ne z ESP8266
- zem zdroje LED a zem ESP8266 musí být propojená
- datový vodič každého pásku přiveď na příslušný GPIO pin
- pokud je pásek delší nebo napájený silněji, řeš napájení LED zvlášť a dimenzovaně

## První spuštění

1. Nahraj firmware do ESP8266.
2. Po startu se modul pokusí připojit do uložené WiFi.
3. Pokud se nepřipojí, přejde do AP režimu.
4. V AP režimu vytvoří WiFi síť se jménem modulu.
5. Otevři v telefonu nebo PC tuto WiFi a přejdi na web modulu.

Jméno modulu je standardně `ESP8266_Modul`, pokud v EEPROM není platná konfigurace.

Poznámka:

- po změně formátu konfigurace v EEPROM se při prvním startu použijí výchozí hodnoty a uloží se nový formát konfigurace

## Webové rozhraní

Na hlavní stránce lze nastavit:

- jméno modulu
- WiFi SSID
- WiFi heslo
- volbu DHCP nebo pevné IP
- pevnou IP adresu, bránu, masku a DNS při vypnutém DHCP
- počet aktivních pásků 1 až 4
- maximální jas 1 až 255
- počet LED pro každý pásek
- rychlost rozsvícení pro každý pásek v ms na jednu LED

Po stisku `Uložit a Restartovat` se konfigurace uloží a modul se restartuje.

Na stejné stránce jsou i testovací tlačítka:

- ON pro konkrétní pásek
- OFF pro konkrétní pásek
- Zapnout vše
- Vypnout vše

## Chování sítě

Modul běží ve dvou režimech:

- STA režim: připojí se do domácí WiFi a použije DHCP nebo pevnou IP podle konfigurace
- AP režim: vytvoří vlastní WiFi, pokud se nepodaří připojit do uložené sítě

Aktuální IP adresa je vypsaná dole na webové stránce modulu.

Poznámka:

- volba DHCP nebo pevné IP se používá pro STA režim při připojení do domácí WiFi
- v AP režimu si modul vytvoří vlastní přístupový bod bez této STA adresy

Pokud je zapnuté DHCP:

- IP adresu přidělí router
- v Loxone je vhodné udělat rezervaci DHCP podle MAC adresy zařízení

Pokud je DHCP vypnuté:

- modul použije ručně zadanou IP adresu
- nastav správně IP, bránu, masku a DNS pro svou síť
- pro Loxone je to jednodušší, protože adresa modulu zůstane stálá

## HTTP ovládání

Firmware má aktuálně tyto HTTP endpointy:

- `GET /` zobrazí konfigurační stránku
- `POST /save` uloží konfiguraci a restartuje modul
- `GET /on?id=1` zapne pásek 1
- `GET /off?id=1` vypne pásek 1
- `GET /on?id=0` zapne všechny aktivní pásky
- `GET /off?id=0` vypne všechny aktivní pásky

Poznámky:

- parametr `id` je v rozsahu 1 až 4 pro jednotlivé pásky
- hodnota `0` znamená všechny aktivní pásky
- odpověď na ON/OFF je prosté `OK`
- firmware zatím nemá autorizaci
- firmware zatím nemá stavový JSON endpoint

## Sériová linka

Monitor běží na 115200 baud.

Podporované příkazy:

- `on` zapne všechny aktivní pásky
- `on 1` zapne pásek 1
- `off` vypne všechny aktivní pásky
- `off 1` vypne pásek 1
- `status` vypíše stav pásků
- `info` vypíše konfiguraci a síť
- `restart` restartuje modul
- `reset` smaže EEPROM a restartuje modul

## Integrace do Loxone

Pro současný stav projektu je nejjednodušší použít HTTP volání přes virtuální výstupy.

### Varianta 1: jednoduché ON a OFF

1. V Loxone Config přidej `Virtuální výstup`.
2. Nastav adresu na IP modulu, například `http://192.168.1.50`.
3. Vytvoř příkazy pro zapnutí a vypnutí.
4. HTTP metoda při zapnutí i vypnutí je `GET`.

Příklady příkazů:

- pásek 1 ON: `/on?id=1`
- pásek 1 OFF: `/off?id=1`
- vše ON: `/on?id=0`
- vše OFF: `/off?id=0`

Tyto příkazy potom připoj na tlačítka, výstupy nebo automatiky v Loxone.

### Varianta 2: jeden pár příkazů pro každý pásek

Pro 4 pásky si v Loxone vytvoř tyto HTTP příkazy:

- `/on?id=1`
- `/off?id=1`
- `/on?id=2`
- `/off?id=2`
- `/on?id=3`
- `/off?id=3`
- `/on?id=4`
- `/off?id=4`

### Doporučené použití v Loxone

- pro manuální ovládání použij tlačítko nebo spínač a zavolej odpovídající URL
- pro centrální zhasnutí domu použij `/off?id=0`
- pro světelné scény si udělej samostatné akce pro jednotlivé pásky

## Omezení aktuální verze

- není k dispozici JSON API
- není k dispozici autorizace
- není k dispozici stavový feedback přes HTTP pro Loxone
- Loxone umí modul ovládat, ale neověřuje skutečný stav přes samostatný endpoint

## Doporučený postup pro zprovoznění s Loxone

1. Nahraj firmware.
2. Připoj modul do WiFi přes webovou stránku.
3. Rozhodni, jestli bude modul používat DHCP nebo pevnou IP.
4. Ověř v prohlížeči, že funguje `http://IP_MODULU/on?id=1` a `http://IP_MODULU/off?id=1`.
5. V Loxone Config založ virtuální výstup s IP adresou modulu.
6. Přidej HTTP příkazy pro ON a OFF.
7. Připoj příkazy na tlačítka nebo logiku.
8. Nakonec otestuj centrální vypnutí přes `/off?id=0`.

## Kde je to v kódu

- validace a výchozí konfigurace EEPROM: [runner/runner.ino](runner/runner.ino#L145) a [runner/runner.ino](runner/runner.ino#L265)
- nastavení DHCP nebo pevné IP: [runner/runner.ino](runner/runner.ino#L319)
- webové rozhraní: [runner/runner.ino](runner/runner.ino#L340)
- sériové příkazy: [runner/runner.ino](runner/runner.ino#L519)
- inicializace WiFi a režim AP/STA: [runner/runner.ino](runner/runner.ino#L627)
- HTTP endpointy: [runner/runner.ino](runner/runner.ino#L667)
