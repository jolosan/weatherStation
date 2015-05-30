/* 
 
 Este código lee diferentes sensores (wind speed, direction, rain gauge, humidty, pressure, light, batt_lvl)
 e informa a través del puerto serie. Esto puede ser fácilmente enrutado a una página web (such as OpenLog) o a un transmisor wireless
 (such as Electric Imp).
 
 Las medidas son mostradas cada segundo, pero la velocidad del viento y la medida de lluvia 
 están ligados a las interrupciones que son calculadas en cada informe
 
 */

#include <Wire.h> //I2C needed for sensors
#include <SPI.h>
#include <Ethernet.h>		//Ethernet
#include "MPL3115A2.h" //Pressure sensor
#include "HTU21D.h" //Humidity sensor


MPL3115A2 myPressure; // Crea una instancia del sensor de presion atmosférica
HTU21D myHumidity; // Crea una instancia del sensor de humedad

// Configuracin de la red --------------------

// direccion MAC del escudo Ethernet
// Pon la direccion correcta aqui:
byte mac[] = { 
  0x90, 0xA2, 0xDA, 0x0F, 0xC6, 0x8E};  // MAC del ethernet Shield de ARDUINO

EthernetClient client;


// ---------------------------------------------------

// PROGMEM Store data in flash (program) memory instead of SRAM. 
// ------ Configuracion envio a wunderground
char SERVER1 [] = "weatherstation.wunderground.com";  // Standard server
char SERVER2 [] = "data.sparkfun.com";  // Standard server
char SERVER3 [] = "www.devicehub.net";  // Standard server
//----------------------------------------------------
unsigned int connections = 0;           // number of connections
unsigned int timeout = 30000;           // Milliseconds -- 1000 = 1 Second
unsigned long lastConnectionTime = 0;             // last time you connected to the server, in milliseconds
boolean lastConnected = false;
const unsigned long postingInterval = 60L*1000L;  // delay between updates (60 seconds)

// Asignaciones de pines
//-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=
// Pines digitales de E/S
const byte WSPEED  = 3;
const byte RAIN  = 2;
const byte STAT1 = 7;
const byte STAT2 = 8;

// Pines analogicos de E/S
const byte REFERENCE_3V3 = A3;
const byte LIGHT = A1;
const byte BATT = A2;
const byte WDIR = A0;
//-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=

// Variables globales
//-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=
long lastSecond; // Contador de milisegundos para ver cuando transcurre un segundo
byte seconds; // Cuando alcanza 60, incrementa el minuto actual
byte seconds_2m; // Lleva la cuenta del "wind speed/dir avg" sobre los datos del array de los 2 ultimos minutos
byte minutes; // Lleva la cuenta de donde estamos en los diferentes arrays de datos
byte minutes_10m; // Lleva la cuenta de donde estamos en ráfaga de viento/direccion sobre los 10 ultimos minutos del array de datos

long lastWindCheck = 0;
volatile long lastWindIRQ = 0;
volatile byte windClicks = 0;

// Necesitamos llevar la cuenta de las siguientes variables:
// Wind speed/dir each update (no storage)
// Wind gust/dir over the day (no storage)
// Wind speed/dir, avg over 2 minutes (store 1 per second)
// Wind gust/dir over last 10 minutes (store 1 per minute)
// Rain over the past hour (store 1 per minute)
// Total rain over date (store one per day)

byte windspdavg[120]; // 120 bytes para almacenar la media de 2 minutos
int winddiravg[120]; // 120 ints para almacenar la media de 2 minutos 
float windgust_10m[10]; // 10 floats para almacenar los max durante 10 minutos
int windgustdirection_10m[10]; // 10 ints para almacenar los max durante 10 minutos
volatile float rainHour[60]; // 60 floating numbers para almacenar los 60 minutos de lluvia

// Estos son todos los valores del tiempo que espera wunderground

int winddir = 0; // [0-360 direccion instantánea del viento]
float windspeedmph = 0; // [mph velocidad instantanea del viento]
float windgustmph = 0; // [mph rafaga de viento actual, usando el periodo de tiempo específico por software]
int windgustdir = 0; // [0-360 usando el periodo de tiempo especifico por software]
float windspdmph_avg2m = 0; // [mph velocidad media del viento durante 2 minutos mph]
int winddir_avg2m = 0; // [0-360 direccion media del viento durante 2 minutos]
float windgustmph_10m = 0; // [mph ráfaga de viento de los 10 ultimos minutos mph]
int windgustdir_10m = 0; // [0-360 direccion del viento de los 10 ultimos minutos]
float humidity = 0; // [%]
float tempf = 0; // [temperatura ºF]
float rainin = 0; // [pulgadas de lluvia durante la pasada hora)] -- la lluvia acumulada en los ultimos 60 min
volatile float dailyrainin = 0; // [pulgadas de lluvia hasta ahora en tiempo local]
float baromin = 0; // [barom in] - Es difícil calcular el baromin localmente, hacerlo en el agente
                   // pressure*0.0002953  Calc for converting Pa to inHg (wunderground)
float pressure = 0;
// float dewptf; // [dewpoint F] - Es difícil calcular el dewpoint localmente, hacerlo en el agente

float batt_lvl = 11.8; //[valor analogico entre 0 y 1023]
float light_lvl = 455; //[valor analogico entre 0 y 1023]

// volatiles estan sujetos a modificacion por las IRQs
volatile unsigned long raintime, rainlast, raininterval, rain;


//-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=

// rutinas de interrupción (son llamadas por interrupciones HW, no por el código principal)
//-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=
void rainIRQ()
// Cuenta los volcados del cubo pluviómetro que se producen
// Activados por el imán y el reed switch en el pluviómetro, enlazados a la entrada D2
{
  raintime = millis(); // toma el tiempo actual
  raininterval = raintime - rainlast; // calcula el intervalo entre éste y el último evento

    if (raininterval > 10) // ignore switch-bounce glitches less than 10mS after initial edge
  {
    dailyrainin += 0.011; // Cada volcado es 0.011" de agua
    rainHour[minutes] += 0.011; // Incremente este minuto la cantidad de lluvia

    rainlast = raintime; // configuralo para el siguiente evento
  }
}

void wspeedIRQ()
// Activado por el imán en el anemómetro (2 ticks por rotación), enlazado a la entrada D3
{
  if (millis() - lastWindIRQ > 10) // Ignore switch-bounce glitches less than 10ms (142MPH max reading) after the reed switch closes
  {
    lastWindIRQ = millis(); // Toma el tiempo actual
    windClicks++; // Hay 1.492MPH por cada click por segundo.
  }
}


void setup()
{
  Serial.begin(9600);
  
  if (Ethernet.begin(mac) ) {
    Serial.println(F("Inicializacion completa"));
    // print your local IP address:
    Serial.print(F("Mi direccion IP: "));
    for (byte thisByte = 0; thisByte < 4; thisByte++) {
      // print the value of each byte of the IP address:
      Serial.print(Ethernet.localIP()[thisByte], DEC);
      Serial.print(F("."));
      }
      Serial.println();
     } 
  else {
    Serial.println(F("Algo fue mal durante el inicio de Ethernet!"));
  	} 

  pinMode(STAT1, OUTPUT); // LED de estado Blue
  pinMode(STAT2, OUTPUT); // LED de estado Green
  
  pinMode(WSPEED, INPUT_PULLUP); // entrada de los medidores del sensor del viento
  pinMode(RAIN, INPUT_PULLUP); // entrada de los medidores del sensor de lluvia
  
  pinMode(REFERENCE_3V3, INPUT);
  pinMode(LIGHT, INPUT);

  // Configura el sensor de presion atmosferica
  myPressure.begin(); // Inicia el sensor
  myPressure.setModeBarometer(); // Mide la presion en Pascales desde 20 hasta 110 kPa
  myPressure.setOversampleRate(7); // Asignar el sobremustreo al recomendado 128
  myPressure.enableEventFlags(); // Habilitar los tres flags de presión y enventos de temperatura 

  //Configura el sensor de humedad
  myHumidity.begin();

  seconds = 0;
  lastSecond = millis();

  // enlaza pines de interrupciones externas a funciones IRQ
  attachInterrupt(0, rainIRQ, FALLING);
  attachInterrupt(1, wspeedIRQ, FALLING);

  // activa las interrupciones
  interrupts();
   

  Serial.println(F("Escudo del clima online!"));

}

void loop()
{
  // Lleva la cuenta de que minuto es
  if(millis() - lastSecond >= 1000)
  {
    digitalWrite(STAT1, HIGH); // Parpadea el LED de estado
    
    lastSecond += 1000;

    // Toma una lectura de la velocidad y dirección cada segundo para una media de 2 minutos    if(++seconds_2m > 119) seconds_2m = 0;

    // Calcula la velocidad y dirección del viento cada segundo durante 120 segundos para obtener la media de 2 minutos
    float currentSpeed = get_wind_speed();
    // float currentSpeed = random(5); // Para testear
    int currentDirection = get_wind_direction();
    windspdavg[seconds_2m] = (int)currentSpeed;
    winddiravg[seconds_2m] = currentDirection;
    //if(seconds_2m % 10 == 0) displayArrays(); // Para testear

    // Comprueba par ver si esto es una réfaga para el minuto
    if(currentSpeed > windgust_10m[minutes_10m])
    {
      windgust_10m[minutes_10m] = currentSpeed;
      windgustdirection_10m[minutes_10m] = currentDirection;
    }

    // Comprueba par ver si esto es una réfaga para el dia
    if(currentSpeed > windgustmph)
    {
      windgustmph = currentSpeed;
      windgustdir = currentDirection;
    }

    if(++seconds > 59)
    {
      seconds = 0;

      if(++minutes > 59) minutes = 0;
      if(++minutes_10m > 9) minutes_10m = 0;

      rainHour[minutes] = 0; // Inicializa a cero la cantidad de lluvia de este minuto
      windgust_10m[minutes_10m] = 0; // Inicializa a cero la ráfaga de éste minuto
    }
    
  calcWeather();
  // Informa de todas las lecturas por el puerto serie
  //printWeather();
  // sendWeatherToWunderground();
    
    digitalWrite(STAT1, LOW); // Apaga el LED de estado
  }

 // Envio de datos ....
  if (client.available()) {
    char c = client.read();
    Serial.print(c);
  }

  // if there's no net connection, but there was one last time
  // through the loop, then stop the client:
  if (!client.connected() && lastConnected) {
    Serial.println(F("Desconectando..."));
    client.stop();
  }

  // if you're not connected, and ten seconds have passed since
  // your last connection, then connect again and send data:
  if(!client.connected() && (millis() - lastConnectionTime > postingInterval)) {
    Serial.println("------- Subiendo los datos  a Wunderground -------");
    sendDataToWunderground();
    //Serial.println("wunderground.com devolvio... ");
    Serial.println("------- Subiendo los datos  a Sparkfun -------");
    sendDataToSparkfun();
    //Serial.println("Sparkfun devolvio... ");
    Serial.println("------- Subiendo los datos  a DeviceHUB -------");
    sendDataToDeviceHUB();
    //Serial.println("DeviceHUB devolvio... ");
  }
  // store the state of the connection for next time through
  // the loop:
  lastConnected = client.connected();
}

//Calcula cada una de las variables que espera  wunderground 
void calcWeather()
{
  //Calc winddir
  winddir = get_wind_direction();

  //Calc windspeed
  //windspeedmph = get_wind_speed();
   windspeedmph = 0; //hasta que funcione correctamente la placa 
   
  //Calc windgustmph
  //Calc windgustdir
  // Informa de la ráfaga más larga hoy
  windgustmph = 0;
  windgustdir = 0;

  //Calc windspdmph_avg2m
  float temp = 0;
  for(int i = 0 ; i < 120 ; i++)
    temp += windspdavg[i];
  temp /= 120.0;
  windspdmph_avg2m = temp;

  //Calc winddir_avg2m
  temp = 0; // No puedo usar winddir_avg2m porque es un int
  for(int i = 0 ; i < 120 ; i++)
    temp += winddiravg[i];
  temp /= 120;
  winddir_avg2m = temp;

  //Calc windgustmph_10m
  //Calc windgustdir_10m
  // Encuentra la ráfaga más larga en los últimos 10 minutos
  windgustmph_10m = 0;
  windgustdir_10m = 0;
  // Recorre los 10 minutos  
  for(int i = 0; i < 10 ; i++)
  {
    if(windgust_10m[i] > windgustmph_10m)
    {
      windgustmph_10m = windgust_10m[i];
      windgustdir_10m = windgustdirection_10m[i];
    }
  }

  //Calc humidity
  humidity = myHumidity.readHumidity();
  //float temp_h = myHumidity.readTemperature();
  //Serial.print(" TempH:");
  //Serial.print(temp_h, 2);

  //Calc tempf from pressure sensor
  tempf = myPressure.readTempF();
  //Serial.print(" TempP:");
  //Serial.print(tempf, 2);

  // La lluvia total del día se calcula dentro de la interrupción 
  // Calcula la cantidad de lluvia durante los últimos 60 minutos
  rainin = 0;  
  for(int i = 0 ; i < 60 ; i++)
    rainin += rainHour[i];

  //Calc pressure
  pressure = myPressure.readPressure();

  //Calc baromin
  baromin = pressure *0.0002953;

  //Calc light level
  light_lvl = get_light_level();

  //Calc battery level
  batt_lvl = get_battery_level();
}

// Devuelve el voltaje del sensor de luz basado en el 3.3V rail
// Esto nos permite ignorar qué podría ser VCC (an Arduino plugged into USB has VCC of 4.5 to 5.2V)
float get_light_level()
{
  float operatingVoltage = analogRead(REFERENCE_3V3);

  float lightSensor = analogRead(LIGHT);
  
  operatingVoltage = 3.3 / operatingVoltage; // El voltaje de referencia es 3.3V
  
  lightSensor = operatingVoltage * lightSensor;
  
  return(lightSensor);
}

// Devuelve el voltaje del sensor del e raw pin based on the 3.3V rail
// Esto nos permite ignorar qué podría ser VCC (an Arduino plugged into USB has VCC of 4.5 to 5.2V)
// El nivel de batería está conectado al pin RAW en Arduino y es alimentado a por dos resistencias del 5%:
//3.9K el la parte alta (R1), y 1K en la parte baja (R2)
float get_battery_level()
{
  float operatingVoltage = analogRead(REFERENCE_3V3);

  float rawVoltage = analogRead(BATT);
  
  operatingVoltage = 3.30 / operatingVoltage; // El voltaje de referencia es 3.3V
  
  rawVoltage = operatingVoltage * rawVoltage; // Convierte el 0 a 1023 int al voltaje real en el pin BATT
  
  rawVoltage *= 4.90; //(3.9k+1k)/1k - multiple BATT voltaje por el divisor de voltaje para obtener el voltaje del sistema real
  
  return(rawVoltage);
}

// Devuelve la velocidad del viento instantánea
float get_wind_speed()
{
  float deltaTime = millis() - lastWindCheck; //750ms

  deltaTime /= 1000.0; //Convierte a segundos

  float windSpeed = (float)windClicks / deltaTime; //3 / 0.750s = 4

  windClicks = 0; // Inicializa e inicia a buscar nuevo viento
  lastWindCheck = millis();

  windSpeed *= 1.492; //4 * 1.492 = 5.968MPH

  /* Serial.println();
   Serial.print("Windspeed:");
   Serial.println(windSpeed);*/

  return(windSpeed);
}

// Lee el sensor de la dirección del viento, devuelve la cabecera en grados
int get_wind_direction() 
{
  unsigned int adc;

  adc = analogRead(WDIR); // obtén la lectura actual del sensor

  // La siguente tabla es de lecturas ADC de la salida del sensor de dirección del viento, ordenadas de bajo a alto.
  // Cada umbral es el punto medio entre encabezados adyacentes. La salida es grados para esa lectura ADC.
  // Notar que no están en órdenes de grados de una brújula! Ver la hoja de datos de los Weather Meters para más informació.

  if (adc < 380) return (113);
  if (adc < 393) return (68);
  if (adc < 414) return (90);
  if (adc < 456) return (158);
  if (adc < 508) return (135);
  if (adc < 551) return (203);
  if (adc < 615) return (180);
  if (adc < 680) return (23);
  if (adc < 746) return (45);
  if (adc < 801) return (248);
  if (adc < 833) return (225);
  if (adc < 878) return (338);
  if (adc < 913) return (0);
  if (adc < 940) return (293);
  if (adc < 967) return (315);
  if (adc < 990) return (270);
  return (-1); // error, desconectado?
}


// Imprime las diferentes variables directamente al puerto
// No me gusta la forma en que está escrita esta función, pero Arduino no soporta escribir floats bajo sprintf
/*
void printWeather()
{
    Serial.print(F("&winddir="));
    Serial.print(winddir);
    Serial.print(F("&windspeedmph="));
    Serial.print(windspeedmph);
    Serial.print(F("&windgustmph="));
    Serial.print(windgustmph);
    Serial.print(F("&windgustdir="));
    Serial.print(windgustdir);
    Serial.print(F("&windspdmph_avg2m="));
    Serial.print(windspdmph_avg2m);
    Serial.print(F("&winddir_avg2m="));
    Serial.print(winddir_avg2m);
    Serial.print(F("&windgustmph_10m="));
    Serial.print(windgustmph_10m);
    Serial.print(F("&windgustdir_10m="));
    Serial.print(windgustdir_10m);
    Serial.print(F("&humidity="));
    Serial.print(humidity);
    Serial.print(F("&tempf="));
    Serial.print(tempf);
    Serial.print(F("&rainin="));
    Serial.print(rainin);
    Serial.print(F("&dailyrainin="));
    Serial.print(dailyrainin);
    Serial.print(F("&pressure="));
    Serial.print(pressure);
    Serial.print(F("&batt_lvl="));
    Serial.print(batt_lvl);
    Serial.print(F("&light_lvl="));
    Serial.print(light_lvl);
    Serial.print(F("&baromin="));
    Serial.print(baromin);
    Serial.println("---- -------------- ----");
    
}
*/

// this method makes a HTTP connection to the wunderground server:
void sendDataToWunderground() {

  // if there's a successful connection:
  if (client.connect(SERVER1, 80)) {
    
    // send the HTTP PUT request:
    client.print(F("GET /weatherstation/updateweatherstation.php?ID=XXXXXXXXXXXXX"));
    client.print(F("&PASSWORD=YYYYYYYYYYY"));
    client.print(F("&dateutc=now"));
    client.print(F("&winddir="));
    client.print(winddir);
    client.print(F("&windspeedmph="));
    client.print(windspeedmph);
    client.print(F("&windgustmph="));
    client.print(windgustmph);
    client.print(F("&windgustdir="));
    client.print(windgustdir);
    client.print(F("&windspdmph_avg2m="));
    client.print(windspdmph_avg2m);
    client.print(F("&winddir_avg2m="));
    client.print(winddir_avg2m);
    client.print(F("&windgustmph_10m="));
    client.print(windgustmph_10m);
    client.print(F("&windgustdir_10m="));
    client.print(windgustdir_10m);
    client.print(F("&humidity="));
    client.print(humidity);
    client.print(F("&tempf="));
    client.print(tempf);
    client.print(F("&rainin="));
    client.print(rainin);
    client.print(F("&dailyrainin="));
    client.print(dailyrainin);
    client.print(F("&baromin="));
    client.print(baromin);
    client.print(F("&action=updateraw"));
    client.println(F(" HTTP/1.1"));
    client.println(F("Host: weatherstation.wunderground.com"));
    client.print(F("User-Agent: "));
    client.println(F("wunderground"));
    client.println(F("Connection: close"));
    client.println();

  }
  else {
    // if you couldn't make a connection:
    Serial.println(F("connection failed"));
    Serial.println();
    Serial.println(F("disconnecting."));
    client.stop();
  }
  // note the time that the connection was made or attempted:
  lastConnectionTime = millis();
}

// this method makes a HTTP connection to the Saprkfun server:
void sendDataToSparkfun() {

  
  // if there's a successful connection:
  if (client.connect(SERVER2, 80)) {
   // send the HTTP PUT request:
   
    client.print(F("POST /input/XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX"));
    client.println(F(" HTTP/1.1"));
    client.println(F("Host: data.sparkfun.com"));
    client.println(F("Phant-Private-Key: YYYYYYYYYYYYYYYYYYYYYYYYYYYYYYYYYY"));
    client.println(F("Connection: close"));
    client.println(F("Content-Type: application/x-www-form-urlencoded"));
    client.println(F("Content-Length: 206"));
    client.println();
    client.print(F("winddir="));
    client.print(winddir);
    client.print(F("&windspeedmph="));
    client.print(windspeedmph);
    client.print(F("&windgustmph="));
    client.print(windgustmph);
    client.print(F("&windgustdir="));
    client.print(windgustdir);
    client.print(F("&windspdmph_avg2m="));
    client.print(windspdmph_avg2m);
    client.print(F("&winddir_avg2m="));
    client.print(winddir_avg2m);
    client.print(F("&windgustmph_10m="));
    client.print(windgustmph_10m);
    client.print(F("&windgustdir_10m="));
    client.print(windgustdir_10m);
    client.print(F("&humidity="));
    client.print(humidity);
    client.print(F("&tempf="));
    client.print(tempf);
    client.print(F("&rainin="));
    client.print(rainin);
    client.print(F("&dailyrainin="));
    client.print(dailyrainin);
    client.print(F("&baromin="));
    client.print(baromin);  
  }
  
   else {
    // if you couldn't make a connection:
    Serial.print(F("Connection to ")); 
    Serial.print(SERVER2);
    Serial.println(F(" failed"));
    Serial.println();
    Serial.println(F("Disconnecting from Sparkfun."));
    client.stop();
  }
}


// this method makes a HTTP connection to the devicehub server:
void sendDataToDeviceHUB() {

  // if there's a successful connection:
  if (client.connect(SERVER3, 80)) {
    client.print(F("GET /io/YYYY/?apiKey=XXXXXXXXXXXX"));
    client.print(F("&Direccion_Viento="));
    client.print(winddir);
    client.print(F("&Velocidad_Viento="));
    client.print(windspeedmph);
    client.print(F("&Humedad="));
    client.print(humidity);
    client.print(F("&Temperatura="));
    client.print(tempf);
    client.print(F("&Lluvia="));
    client.print(rainin);
    client.print(F("&Presion_Atmosferica="));
    client.print(baromin);
    client.println(F(" HTTP/1.1"));
    client.println(F("Host: www.devicehub.net"));
    client.print(F("User-Agent: "));
    client.println(F("devicehub"));
    client.println(F("Connection: close"));
    client.println();
  }
   else {
    // if you couldn't make a connection:
    Serial.println(F("Connection to DeviceHUB.net failed"));
    Serial.println();
    Serial.println(F("Disconnecting from DeviceHUB.net."));
    client.stop();
  }
 
}
