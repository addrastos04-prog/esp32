// ========================================================================================================================
//  LIBRERÍAS
// ========================================================================================================================

#define ENABLE_SERVICE_AUTH
#define ENABLE_FIRESTORE

#include <WiFi.h>
#include <time.h>
#include <FirebaseClient.h>
#include <WiFiClientSecure.h>

// ========================================================================================================================
//  WIFI Y NTP
// ========================================================================================================================




// ========================================================================================================================
//  CREDENCIALES FIREBASE
// ========================================================================================================================




// ========================================================================================================================
//  OBJETOS FIREBASE
// ========================================================================================================================

WiFiClientSecure ssl_client;
WiFiClientSecure ssl_client_cmd;

using AsyncClient = AsyncClientClass;
AsyncClient aClient(ssl_client);
AsyncClient aClientCmd(ssl_client_cmd);

FirebaseApp app;
ServiceAuth sa_auth(FIREBASE_CLIENT_EMAIL, FIREBASE_PROJECT_ID, FIREBASE_PRIVATE_KEY, 3000);
Firestore::Documents Docs;

//=======================================================================================================================
// Definicion de pines del ESP32
//=======================================================================================================================

#define BUZZER_PIN 27  // buzzer

// ========================================================================================================================
//  CONSTANTES
// ========================================================================================================================

const unsigned long INTERVALO_NTP = 10000;
const unsigned long INTERVALO_REVISION_COMANDO = 3000; // revisa cada 3 segundos

// ========================================================================================================================
//      VARIABLES GLOBALES
// ========================================================================================================================

unsigned long ultimaSincNTP = 0;
unsigned long ultimaRevisionComando = 0;
bool accesoYaEscrito = false;
struct tm timeinfoDelEvento;

// ========================================================================================================================
//      VARIABLES DE CONTROL DE ACCESO
// ========================================================================================================================

int    fingerprintIdPendiente   = 0;
String nombreCompletoEncontrado = "";   // nombre legible
String tipoAccesoPendiente = "entrada";
Document<Values::Value> docEstadoGlobal;
bool accesoPatchResuelto = false;
bool estadoPatchResuelto = false;


// ========================================================================================================================
//    VARIABLES DE ENROLAMIENTO
// ========================================================================================================================
String nombrePendienteEnrolar = "";
String documentoPendienteEnrolar = "";

// ========================================================================================================================
//    ESTADOS DE FLUJO DE ACCESO
// ========================================================================================================================
// Nota: el ESP32 ya no calcula jornadas ni horas trabajadas.
// Solo registra el evento (entrada/salida) con su timestamp en Firestore.
// Ese calculo ahora se hace en la app Flutter.

enum EstadoFlujo {

  ESPERANDO_INPUT,            // Esperando una nueva huella

  BUSCANDO_USUARIO,           // Consulta el usuario en Firestore

  LEYENDO_ESTADO,             // Lee si el usuario está dentro o fuera

  ACTUALIZANDO_ESTADO,        // Actualiza estadoAcceso

  ESPERANDO_PATCH,            // Espera la respuesta del patch

  REVISANDO_COMANDOS,         // Revisa comandos pendientes

  ESPERANDO_ID_ENROLAMIENTO   // Espera el ID durante el enrolamiento

};

EstadoFlujo estadoActual = ESPERANDO_INPUT;

// ========================================================================================================================
//  FUNCIONES PARA BUZZER
// ========================================================================================================================

void beepPermitido() {
  tone(BUZZER_PIN, 2000);
  delay(300);
  noTone(BUZZER_PIN);
  digitalWrite(BUZZER_PIN, HIGH);
}

void beepDenegado() {
  tone(BUZZER_PIN, 200);
  delay(300);
  noTone(BUZZER_PIN);
  digitalWrite(BUZZER_PIN, HIGH);
}

void beepSolicitarHuella() {
  tone(BUZZER_PIN, 6000);
  delay(300);
  noTone(BUZZER_PIN);
  digitalWrite(BUZZER_PIN, HIGH);
}

// ========================================================================================================================
//  FUNCIONES DE FECHA Y HORA
// ========================================================================================================================

bool obtenerTiempo(struct tm& timeinfo) {
  if (millis() - ultimaSincNTP >= INTERVALO_NTP) {
    configTime(gmtOffset, daylightOffset, ntpServer);
    ultimaSincNTP = millis();
  }
  if (!getLocalTime(&timeinfo)) {
    Serial.println("Error NTP");
    return false;
  }
  return true;
}

String timestampStr(const struct tm& t) {
  char buf[20];
  snprintf(buf, sizeof(buf), "%02d/%02d/%04d %02d:%02d:%02d",
           t.tm_mday, t.tm_mon + 1, t.tm_year + 1900,
           t.tm_hour, t.tm_min, t.tm_sec);
  return String(buf);
}

String construirIdDocumento(const String& nombreId, const struct tm& t) {
  char fechaHora[20];
  snprintf(fechaHora, sizeof(fechaHora), "%04d-%02d-%02d_%02d%02d%02d",
           t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
           t.tm_hour, t.tm_min, t.tm_sec);
  return nombreId + "_" + String(fechaHora);
}

String obtenerTimestampUTC() {
  time_t epochUTC = time(nullptr);
  struct tm utc;
  gmtime_r(&epochUTC, &utc);
  char tsBuf[25];
  snprintf(tsBuf, sizeof(tsBuf), "%04d-%02d-%02dT%02d:%02d:%02dZ",
           utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday,
           utc.tm_hour, utc.tm_min, utc.tm_sec);
  return String(tsBuf);
}

// ========================================================================================================================
//  FUNCIONES AUXILIARES — FIRESTORE
// ========================================================================================================================

// Extrae el valor de un campo stringValue del JSON de Firestore
String extraerStringValue(const String& json, const String& campo) {
  int pos = json.indexOf("\"" + campo + "\"");
  if (pos == -1) return "";
  int posStrVal = json.indexOf("\"stringValue\"", pos);
  if (posStrVal == -1) return "";
  int inicio = json.indexOf("\"", posStrVal + 14) + 1;
  int fin    = json.indexOf("\"", inicio);
  return json.substring(inicio, fin);
}

// ========================================================================================================================
//  FUNCIONES DE ACCESO
// ========================================================================================================================

void escribirAcceso(int fingerprintId, const String& nombre,
                    const String& tipo, const struct tm& timeinfo) {

  accesoPatchResuelto = false;

  // Si es desconocido, el resultado es "denegado"
  String resultado = (nombre == "Desconocido") ? "denegado" : "permitido";

  if (resultado == "permitido") beepPermitido();
  else beepDenegado();

  Document<Values::Value> doc;
  doc.add("fingerprintId", Values::Value(Values::IntegerValue(fingerprintId)));
  doc.add("nombre",        Values::Value(Values::StringValue(nombre)));
  doc.add("resultado",     Values::Value(Values::StringValue(resultado)));
  doc.add("tipo",          Values::Value(Values::StringValue(tipo)));
  doc.add("timestamp",     Values::Value(Values::TimestampValue(obtenerTimestampUTC())));

  String idDoc = construirIdDocumento(nombre, timeinfo);
  Serial.println("Creando acceso (" + tipo + "): " + idDoc);

  Docs.createDocument(aClient,
                      Firestore::Parent(FIREBASE_PROJECT_ID),
                      "accesos/" + idDoc,
                      DocumentMask(),
                      doc,
                      resultadoFirestore);
}

// ========================================================================================================================
//  CALLBACKS FIREBASE
// ========================================================================================================================

// AUTH
void resultadoAuth(AsyncResult &aResult) {
  if (aResult.isError())
    Serial.println("ERROR AUTH: " + String(aResult.error().message().c_str()));
  if (aResult.available())
    Serial.println("Auth OK");
}

// USUARIO
// recibe usuarios/NombreId → extrae nombre completo → listo para escribir
void resultadoUsuario(AsyncResult &aResult) {
  if (aResult.isError()) {
    Serial.println("Usuario no encontrado.");
    nombreCompletoEncontrado = "Desconocido";
    estadoActual = LEYENDO_ESTADO; // aun así registramos el acceso
    return;
  }
  if (aResult.available()) {
    String json = String(aResult.c_str());
    nombreCompletoEncontrado = extraerStringValue(json, "nombre");
    if (nombreCompletoEncontrado == "") nombreCompletoEncontrado = "Desconocido";
    Serial.println("Usuario: " + nombreCompletoEncontrado);
    estadoActual = LEYENDO_ESTADO; // siguiente paso
  }
}

//============================================================
//ESTADO DE ACCESO
//============================================================
void resultadoLeerEstado(AsyncResult &aResult) {
  if (aResult.isError()) {
    tipoAccesoPendiente = "entrada";
    docEstadoGlobal = Document<Values::Value>();
    docEstadoGlobal.add("tipo", Values::Value(Values::StringValue(tipoAccesoPendiente)));
    estadoActual = ACTUALIZANDO_ESTADO;
    return;
  }
  if (aResult.available()) {
    String json = String(aResult.c_str());
    String tipoActual = extraerStringValue(json, "tipo");

    tipoAccesoPendiente = (tipoActual == "entrada") ? "salida" : "entrada";
    Serial.println("Ultimo tipo: " + tipoActual + " -> ahora: " + tipoAccesoPendiente);

    docEstadoGlobal = Document<Values::Value>();
    docEstadoGlobal.add("tipo", Values::Value(Values::StringValue(tipoAccesoPendiente)));
    estadoActual = ACTUALIZANDO_ESTADO;
  }
}


void resultadoActualizarEstado(AsyncResult &aResult) {
  if (estadoPatchResuelto) return;   // ← ignora eventos duplicados/tardíos del mismo patch

  if (aResult.isError()) {
    Serial.println("Error actualizando estado: " + String(aResult.error().message().c_str()));
    estadoPatchResuelto = true;
  } else if (aResult.available()) {
    Serial.println("Estado actualizado correctamente.");
    estadoPatchResuelto = true;
  } else {
    return; // aun no hay respuesta
  }

  escribirAcceso(fingerprintIdPendiente, nombreCompletoEncontrado,
                 tipoAccesoPendiente, timeinfoDelEvento);
  estadoActual = ESPERANDO_INPUT;
}

//============================================================
//ACCESOS
//============================================================
void resultadoFirestore(AsyncResult &aResult) {
  if (accesoPatchResuelto) return;   // ← ignora cualquier evento tardío duplicado

  if (aResult.isError()) {
    Serial.println("Error escribiendo acceso: " + aResult.error().message());
    accesoPatchResuelto = true;
    return;
  }
  if (aResult.available()) {
    Serial.println("✓ Acceso registrado en Firestore.");
    accesoPatchResuelto = true;
  }
}

//============================================================
//COMANDOS
//============================================================
// Callback que procesa la respuesta al leer "comandos/dispositivo1"
void resultadoRevisarComando(AsyncResult &aResult) {
  if (aResult.isError()) return; // si no existe el doc o hay error, simplemente seguimos esperando

  if (aResult.available()) {
    String json = String(aResult.c_str());

    String accion = extraerStringValue(json, "accion");
    String estado = extraerStringValue(json, "estado");

    if (accion == "enrolar_nuevo" && estado == "pendiente") {
      nombrePendienteEnrolar = extraerStringValue(json, "nombreNuevo");
      documentoPendienteEnrolar = extraerStringValue(json, "documentoNuevo");

      if (nombrePendienteEnrolar != "") {
        Serial.println();
        Serial.println("=== NUEVO ENROLAMIENTO SOLICITADO ===");
        Serial.println("Nombre: " + nombrePendienteEnrolar);
        Serial.println("Documento: " + documentoPendienteEnrolar);
        Serial.println("Coloca la huella e ingresa el ID a asignar:");
        beepSolicitarHuella();
        estadoActual = ESPERANDO_ID_ENROLAMIENTO;
      }
    }
  }
}

// Callback de confirmación al escribir el resultado del enrolamiento
void resultadoConfirmarEnrolamiento(AsyncResult &aResult) {
  if (aResult.isError()) {
    Serial.println("Error confirmando enrolamiento: " + String(aResult.error().message().c_str()));
  }
  if (aResult.available()) {
    Serial.println("Enrolamiento confirmado en Firestore.");
    Serial.println();
    Serial.println("=== Listo. Ingresa un fingerprintId (numero) y presiona Enter ===");
  }
}

// ========================================================================================================================
//  SETUP
// ========================================================================================================================

void setup() {
  Serial.begin(115200);
  Serial.println("=== Sistema de Control de Acceso ===");

  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, HIGH);

  WiFi.begin(ssid, password);
  Serial.print("Conectando WiFi");
  while (WiFi.status() != WL_CONNECTED) { delay(500); Serial.print("."); }
  Serial.println("\nIP: " + WiFi.localIP().toString());

  configTime(gmtOffset, daylightOffset, ntpServer);
  struct tm timeinfo;
  Serial.print("Esperando NTP");
  while (!getLocalTime(&timeinfo)) { delay(500); Serial.print("."); }
  Serial.println("\nHora: " + timestampStr(timeinfo));

  ssl_client.setInsecure();
  ssl_client_cmd.setInsecure();
  app.setTime(mktime(&timeinfo));
  initializeApp(aClient, app, getAuth(sa_auth), 120000, resultadoAuth);
  app.getApp<Firestore::Documents>(Docs);

  Serial.println("Firebase listo.");
  Serial.println("Ingresa un fingerprintId y presiona Enter:");

}

// ========================================================================================================================
//  LOOP
// ========================================================================================================================

void loop() {
  app.loop();

  struct tm timeinfo;
  if (!obtenerTiempo(timeinfo)) { delay(1000); return; }

  // Revisión periódica de comandos pendientes (solo si estamos en estado idle)
  if (estadoActual == ESPERANDO_INPUT && millis() - ultimaRevisionComando >= INTERVALO_REVISION_COMANDO) {
    ultimaRevisionComando = millis();
    Docs.get(aClientCmd,
            Firestore::Parent(FIREBASE_PROJECT_ID),
            "comandos/dispositivo1",
            GetDocumentOptions(),
            resultadoRevisarComando);
  }

  switch (estadoActual) {

    // ── idle: esperar número del Serial ──────────────────────
    case ESPERANDO_INPUT:
      if (Serial.available() > 0) {
        String entrada = Serial.readStringUntil('\n');
        entrada.trim();
        int id = entrada.toInt();
        if (id > 0) {
          Serial.println("\n>>> Huella ID: " + String(id));
          Serial.println(">>> Hora: " + timestampStr(timeinfo));
          fingerprintIdPendiente   = id;
          timeinfoDelEvento        = timeinfo;
          nombreCompletoEncontrado = "";
          tipoAccesoPendiente      = "entrada";

          Docs.get(aClient,
                  Firestore::Parent(FIREBASE_PROJECT_ID),
                  "usuarios/" + String(id),
                  GetDocumentOptions(),
                  resultadoUsuario);

          estadoActual = BUSCANDO_USUARIO;
        } else {
          Serial.println("Entrada invalida. Escribe solo un numero.");
        }
      }
      break;

    case ESPERANDO_ID_ENROLAMIENTO:
      if (Serial.available() > 0) {
        String entrada = Serial.readStringUntil('\n');
        entrada.trim();
        int idAsignado = entrada.toInt();

        if (idAsignado > 0) {
          Serial.println(">>> Asignando fingerprintId " + String(idAsignado) + " a " + nombrePendienteEnrolar);

          Document<Values::Value> docConfirmacion;
          docConfirmacion.add("fingerprintId", Values::Value(Values::IntegerValue(idAsignado)));
          docConfirmacion.add("estado", Values::Value(Values::StringValue("completado")));

          Docs.patch(aClient,
                    Firestore::Parent(FIREBASE_PROJECT_ID),
                    "comandos/dispositivo1",
                    PatchDocumentOptions(
                      DocumentMask("fingerprintId,estado"),
                      DocumentMask(),
                      Precondition()
                    ),
                    docConfirmacion,
                    resultadoConfirmarEnrolamiento);

          estadoActual = ESPERANDO_INPUT;
        } else {
          Serial.println("ID invalido. Ingresa solo un numero.");
        }
      }
      break;

    case BUSCANDO_USUARIO:
      // resultadoUsuario cambia el estado cuando termina
      break;

    case LEYENDO_ESTADO:
      Docs.get(aClient,
              Firestore::Parent(FIREBASE_PROJECT_ID),
              "estadoAcceso/" + String(fingerprintIdPendiente),
              GetDocumentOptions(),
              resultadoLeerEstado);

      estadoActual = BUSCANDO_USUARIO; // reutilizamos para "esperando respuesta"
      break;

    case ACTUALIZANDO_ESTADO: {
      Document<Values::Value> docEstado;
      docEstado.add("tipo", Values::Value(Values::StringValue(tipoAccesoPendiente)));

      Docs.patch(aClient,
                Firestore::Parent(FIREBASE_PROJECT_ID),
                "estadoAcceso/" + String(fingerprintIdPendiente),
                PatchDocumentOptions(
                  DocumentMask("tipo"),
                  DocumentMask(),
                  Precondition()
                ),
                docEstado,
                resultadoActualizarEstado);

      estadoActual = ESPERANDO_PATCH;
      break;
    }

    case ESPERANDO_PATCH:
      // resultadoActualizarEstado cambia el estado cuando termina
      break;

  }
}