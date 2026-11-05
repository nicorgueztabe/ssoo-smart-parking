#include "queue.h"
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>// Funciones para cadenas de texto
#include <ctype.h>// Comprobar de que tipo son los caracteres


#define NUM_GATES 4
#define NUM_PRIORITIES 3
#define NUM_EXIT_GATES 2
#define NUM_ENTRY_THREADS (NUM_GATES * NUM_PRIORITIES)
#define CAPACIDAD_OG_CSV 50   // Tamano inicial para el vector car del archivo CSV


// Inicializamos una estructura compartida llamada Lot que contiene la configuracion del parking
typedef struct {
  int capacity;
  int occupied;
  pthread_mutex_t mutex;
  pthread_cond_t cond[NUM_PRIORITIES];
  int waiting[NUM_PRIORITIES];
} Lot;


Lot parking_lot;

// Conexion entre los hilos de entrada y las colas de salida
queue *exit_queue_global[NUM_EXIT_GATES];


// Argumentos para los hilos de entrada
typedef struct {
  int gate;
  int priority;
  queue *entry_lane_queue;    // Cola circular de entrada asignada
} ThreadArgs;


struct car *vector = NULL;


// Variables necesarias para la creacion de hilos
pthread_mutex_t mutex;
pthread_cond_t condicion;
int bandera = 0;


// Declaracion de arrays para la creacion de hilos
// Arrays necesarios para guardar los identificadores de cada hilo
pthread_t entry_ID[NUM_ENTRY_THREADS];
pthread_t exit_ID[NUM_EXIT_GATES];

// Arrays necesarios para el almacenamiento de argumentos de cada hilo
ThreadArgs entry_args[NUM_ENTRY_THREADS];
ThreadArgs exit_args[NUM_EXIT_GATES];


// Declaracion de funciones para evitar errores
void * entry_lane_thread(void * arg);
void * exit_gate_thread(void * arg);
int lectura_CSV(char *ruta_fichero);
int validar_matricula(char*matricula);
int leer_linea(int fd, char *linea, ssize_t max_len);
void lot_claim_space(Lot *l, struct car *c);
void lot_release_space(Lot *l, struct car *c);


//hemos creado esta funcion auxiliar para no tener que repetir el mismo
//codigo de liberacion en cada punto donde puede fallar el programa
void liberar_todo(queue *colas_entrada[], int n_entrada, queue *colas_salida[], int n_salida) {

  //vaciamos el vector
  if (vector != NULL) {
    free(vector);
    vector = NULL;
  }

  //liberamos las colas de entrada
  for (int i = 0; i < n_entrada; i++) {
    if (colas_entrada[i] != NULL) queue_destroy(colas_entrada[i]);
  }

  //liberamos las colas de salida
  for (int i = 0; i < n_salida; i++) {
    if (colas_salida[i] != NULL) queue_destroy(colas_salida[i]);
  }

  //destruimos el mutex y variables condicion del parking_lot
  pthread_mutex_destroy(&parking_lot.mutex);

  //destruimos variables condicion de cada prioridad
  for (int i = 0; i < NUM_PRIORITIES; i++) {
    pthread_cond_destroy(&parking_lot.cond[i]);
  }

  //destruimos el mutex y condicion global
  pthread_mutex_destroy(&mutex);
  pthread_cond_destroy(&condicion);
}


// Nuestro main hace la funcion de parking_manager
int main(int argc, char *argv[]) {
  //argv[0]: Llamada al programa
  //argv[1]: La ruta de un fichero de texto en formato CSV
  //argv[2]: el tamano de las colas circulares
  //argv[3]: numeros de plazas del parking (parking_lot.capacity)

  // Antes de comenzar, comprobamos que los argumentos introducidos son los correctos
  if(argc != 4){
    fprintf(stderr, "ERROR: Número incorrecto de argumentos\nRecuerda: ./parking <CSV> <tamaño-cola> <capacidad_parking>\n");
    return -1;
  }

  // Comprobamos los argumetnos introducidos
  long tamano_cola, capacidad_parking;
  char *puntero1; // La funcion strtol va desplazando el puntero hasta \0 (exito) o hasta encontrar un caracter NO numerico (error)
  tamano_cola = strtol(argv[2],&puntero1,10); // Guardamos el tamano de la cola
  if(*puntero1 != '\0'|| tamano_cola <= 0){
    fprintf(stderr,"ERROR: El tamaño de la cola debe ser un número mayor que 0\n");
    return -1;
  }
  capacidad_parking  = strtol(argv[3],&puntero1,10);
  if(*puntero1 != '\0'|| capacidad_parking <= 0){
    fprintf(stderr,"ERROR: La capacidad del parking debe ser un número mayor que 0\n");
    return -1;
  }

  // Inicializamos la estructura compartida
  // Pasamos la capacidad del parking a un numero entero, despreciando los ultimos digitos del long, para que compile sin warnings
  parking_lot.capacity = (int)capacidad_parking;
  parking_lot.occupied = 0; // El parking empieza vacio

  if (pthread_mutex_init(&parking_lot.mutex, NULL) != 0) {
    printf("ERROR: NO se ha podido inicializar el mutex del parking");
    return -1;
  }

  // Inicializamos las variables y los contadores de espera para cada prioridad
  for (int i = 0; i < NUM_PRIORITIES; i++) {
    if (pthread_cond_init(&parking_lot.cond[i], NULL) != 0) {
      printf("ERROR: Fallo al inicializar las variables de condición del parking");
      return -1;
    }
    parking_lot.waiting[i] = 0;
  }

  // Inicializamos 14 colas circulares que contienen los registros de los vehiculos
  // Una cola circular pertenece exclusivamente a un carril de una puerta
  // Empezamos declarando los punteros necesarios para la creacion de las colas
  queue *entry_queue[NUM_ENTRY_THREADS];
  queue *exit_queue[NUM_EXIT_GATES];

  // Ponemos los punteros a NULL para que si algo falla a medio crear,
  //liberar_todo sepa cuales se han creado y cuales no
  for (int i = 0; i < NUM_ENTRY_THREADS; i++) entry_queue[i] = NULL;
  for (int i = 0; i < NUM_EXIT_GATES; i++) exit_queue[i] = NULL;

  // Creamos las doce colas de entrada que necesitamos para el parking mediante un bucle
  for (int i = 0; i < NUM_ENTRY_THREADS; i++){
    entry_queue[i] = queue_init ((int)tamano_cola);
    if (entry_queue[i] == NULL){
      fprintf (stderr, "ERROR: No se ha podido crear la %d cola de entrada \n", i);
      //liberamos todo lo creado
      liberar_todo(entry_queue, NUM_ENTRY_THREADS, exit_queue, NUM_EXIT_GATES);
      return -1;
    }
  }

  // Creamos las dos colas de salida que necesitamos para el parking
  for (int i = 0; i < NUM_EXIT_GATES; i++){
    exit_queue[i] = queue_init ((int)tamano_cola);
    if (exit_queue[i] == NULL){
      fprintf (stderr, "ERROR: No se ha podido crear la %d cola de salida \n", i);
      //liberamos todo lo creado
      liberar_todo(entry_queue, NUM_ENTRY_THREADS, exit_queue, NUM_EXIT_GATES);
      return -1;
    }
  }

  //vinculamos la puerta de salida a la variable global para conectarla con entry_lane_thread
  for (int i = 0; i < NUM_EXIT_GATES; i++){
    exit_queue_global[i] = exit_queue[i];
  }

  // Inicializamos el mutex y la condicion
  // tb realizamos el control de errores
  if (pthread_mutex_init(&mutex, NULL) != 0) {
    fprintf(stderr, "ERROR: fallo al inicializar el mutex global\n");
    liberar_todo(entry_queue, NUM_ENTRY_THREADS, exit_queue, NUM_EXIT_GATES);
    return -1;
  }
  if (pthread_cond_init(&condicion, NULL) != 0) {
    fprintf(stderr, "ERROR: fallo al inicializar la condicion global\n");
    liberar_todo(entry_queue, NUM_ENTRY_THREADS, exit_queue, NUM_EXIT_GATES);
    return -1;
  }

  // Guardamos en memoria los coches extraidos del archivo CSV antes de crear los hilos
  int coches_validos = lectura_CSV(argv[1]);
  if (coches_validos == -1) {
    // lectura_CSV ya reportó el error con perror
    liberar_todo(entry_queue, NUM_ENTRY_THREADS, exit_queue, NUM_EXIT_GATES);
    return -1;
  }
  // Creamos los 12 hilos de entrada
  for (int i = 0; i < NUM_ENTRY_THREADS; i ++){
    // Llenamos el struct de cada hilo antes de crearlo
    entry_args[i].gate = i / NUM_PRIORITIES;          // La puerta destinada al hilo
    entry_args[i].priority = i % NUM_PRIORITIES;      // La prioridad que tiene el hilo
    entry_args[i].entry_lane_queue = entry_queue[i];  // La cola de entrada del hilo

    // Creamos el hilo pasandole las variables del struct creado anterioriormente
    if (pthread_create(&entry_ID[i], NULL, entry_lane_thread, &entry_args[i]) != 0) {
      //tenemos que realizar el control de errores al crear los hilos
      fprintf (stderr, "ERROR: fallo al crear el hilo de entrada %d\n", i);

      //hay que marcar las colas de los hijos creados como terminadas parq que puedan salir de queu_get
      for (int j = 0; j < i; j++) { //recorremos los hilos creado exitosamente
        pthread_mutex_lock(&entry_queue[j]->mutex); //usamos el mutex parqa evitar carreras
        entry_queue[j]->terminado = 1; // y los vamos marcando como terminados
        pthread_cond_broadcast(&entry_queue[j]->cond_not_empty);
        pthread_mutex_unlock(&entry_queue[j]->mutex);
      }

      //unimos todos los hilos que ya han term,iando
      for (int j = 0; j < i; j++) {
        pthread_join(entry_ID[j], NULL);
      }

      //ahora que ya han terminado todos los hilos podemos liberar todo
      liberar_todo(entry_queue, NUM_ENTRY_THREADS, exit_queue, NUM_EXIT_GATES);
      return -1;
    }
  }


  // Creamos los 2 hilos de salida
  for (int i = 0; i < NUM_EXIT_GATES; i ++){
    // Llenamos el struct de cada hilo antes de crearlo
    exit_args[i].gate = i;                             // La puerta de salida destinada al hilo
    exit_args[i].priority = 0;                        // Para la salida no es necesaria la prioridad.
    // Hemos preferido no modificar el struct original y utilizarlo aqui con un valor de prioridad nulo
    exit_args[i].entry_lane_queue = exit_queue[i];  // La cola de salida del hilo

    // Creamos el hilo pasandole las variables del struct creado previamente
    if (pthread_create(&exit_ID[i], NULL, exit_gate_thread, &exit_args[i]) != 0) {
      fprintf (stderr, "ERROR: Fallo al crear el hilo de salida %d\n", i);
      //si ha fallado ahora el programa hay que liberar los 12 hilos de entrada ya creados
      // y ADEMAS los hilos de salida creados exitosamente

      // recorremos todas las colas de entrada y las marcamos coo termiandas
      for (int j = 0; j < NUM_ENTRY_THREADS; j++) {
        pthread_mutex_lock(&entry_queue[j]->mutex);
        entry_queue[j]->terminado = 1;
        pthread_cond_broadcast(&entry_queue[j]->cond_not_empty);
        pthread_mutex_unlock(&entry_queue[j]->mutex);
      }

      //marcamos las colas de salida que se hayan creado y las marcamos como terminadas
      for (int j = 0; j < i; j++) {
        pthread_mutex_lock(&exit_queue[j]->mutex);
        exit_queue[j]->terminado = 1;
        pthread_cond_broadcast(&exit_queue[j]->cond_not_empty);
        pthread_mutex_unlock(&exit_queue[j]->mutex);
      }

      // Arrancamos los hilos para que puedan terminar
      pthread_mutex_lock(&mutex);
      bandera = 1; //hay que poner la banderqa para que no se queden bloqueados indefinidamente
      pthread_cond_broadcast(&condicion);
      pthread_mutex_unlock(&mutex);

      //esperamos a que terminen todos los hilos de entrada
      for (int j = 0; j < NUM_ENTRY_THREADS; j++) {
        pthread_join(entry_ID[j], NULL);
      }

      //esperamos a que terminen los hilos de salida que se llegaron a crear
      for (int j = 0; j < i; j++) {
        pthread_join(exit_ID[j], NULL);
      }

      //liberamos todos los recursos
      liberar_todo(entry_queue, NUM_ENTRY_THREADS, exit_queue, NUM_EXIT_GATES);
      return -1;
    }
  }

  // Arrancamos todos los threads cerrando el mutex y realizamos control de errores
  if (pthread_mutex_lock(&mutex) != 0) {
    fprintf(stderr, "ERROR: fallo en pthread_mutex_lock al arrancar hilos\n");
  }

  // Enviamos la senal a todos los threads y realizamos control de errores
  bandera = 1;
  if (pthread_cond_broadcast(&condicion) != 0) {
    fprintf(stderr, "ERROR: fallo en pthread_cond_broadcast al arrancar hilos\n");
  }

  // desbloqueamos todos los threads y realizamos control de errores
  if (pthread_mutex_unlock(&mutex) != 0) {
    fprintf(stderr, "ERROR: fallo en pthread_mutex_unlock al arrancar hilos\n");
  }

  // Rellenamos las colas de entradacon el CSV
  for (int i = 0; i < coches_validos; i++) {
    // Establecemos las colas de entrada e introducimos cada vehiculo en la cola que le corresponde
    // Hay 12 colas de entrada (3 carriles de entrada por cada una de las 4 puertas)
    int indice_correspondencia = vector[i].gate * NUM_PRIORITIES + vector[i].priority;
    // Esta ecuacion reparte los coches en las colas correctas, segun su puerta y prioridad
    //realizamos control de errores
    if (queue_put(entry_queue[indice_correspondencia], &vector[i]) != 0) {
      fprintf(stderr, "ERROR: fallo al insertar vehiculo %s en la cola de entrada\n", vector[i].plate);
    }
  }

  // Marcamos las colas de entrada como terminadas y despertamos los que esten esperando en queue_get()
  for (int i = 0; i < NUM_ENTRY_THREADS; i++) {
    pthread_mutex_lock(&entry_queue[i]->mutex);
    entry_queue[i]->terminado = 1;
    pthread_cond_broadcast(&entry_queue[i]->cond_not_empty);
    pthread_mutex_unlock(&entry_queue[i]->mutex);
  }

  // Esperamos a que finalicen todos los hilos
  // Para las colas de entrada
  for (int i = 0; i < NUM_ENTRY_THREADS; i++) {
    //varaible que usaremos pasra gaurdar el valor que devuelve el hilo al terminar
    void *valor_hilo = NULL;
    //res_join guarda el resultado de la llamada pthread_join
    int res_join = pthread_join(entry_ID[i], &valor_hilo);
    //comprobacion de erroes del resultado de pthread_join
    if (res_join != 0) {
      fprintf(stderr,
        "[ERROR][parking_manager] pthread_join fallo para LANE %d GATE %d (codigo: %d)\n",
        entry_args[i].priority, entry_args[i].gate, res_join);
    }
    // Esto indica que hubo un error DENTRO del hilo
    else if (valor_hilo != NULL) {
      fprintf(stderr,
        "[ERROR][parking_manager] LANE %d of GATE %d FINISHED WITH ERROR\n",
        entry_args[i].priority, entry_args[i].gate);
    }
  }
  // Marcamos las colas de salida como terminadas para que los hilos de salida puedan finalizar.
  // Sin esto, queue_get bloquearia para siempre al estar la cola vacia con terminado==0.
  for (int i = 0; i < NUM_EXIT_GATES; i++) {
    pthread_mutex_lock(&exit_queue[i]->mutex);
    exit_queue[i]->terminado = 1;
    pthread_cond_broadcast(&exit_queue[i]->cond_not_empty);
    pthread_mutex_unlock(&exit_queue[i]->mutex);
  }
  //Para las colas de salida
  for (int i = 0; i < NUM_EXIT_GATES; i++) {
    //varaible que usaremos pasra gaurdar el valor que devuelve el hilo al terminar
    void *valor_hilo = NULL;
    //res_join guarda el resultado de la llamada pthread_join
    int res_join = pthread_join(exit_ID[i], &valor_hilo);
    //comprobacion de erroes del resultado de pthread_join
    if (res_join != 0) {
      fprintf(stderr,
        "[ERROR][parking_manager] pthread_join fallo para EXIT GATE %d (codigo: %d)\n",
        exit_args[i].gate, res_join);
    }
    // Esto indica que hubo un error DENTRO del hilo
    else if (valor_hilo != NULL) {
      fprintf(stderr,
        "[ERROR][parking_manager] EXIT GATE %d FINISHED WITH ERROR\n",
        exit_args[i].gate);
    }
  }
  // Liberamos la memoria ocupada por el malloc con su free correspondiente
  free(vector); // Liberamos la memoria del vector de coches
  // Destruimos las colas de entrada
  for (int i = 0; i < NUM_ENTRY_THREADS; i++) {
    queue_destroy(entry_queue[i]);
  }
  // Destruimos las colas salida
  for (int i = 0; i < NUM_EXIT_GATES; i++) {
    queue_destroy(exit_queue[i]);
  }
  // Destruimos el mutex
  pthread_mutex_destroy(&parking_lot.mutex);
  // Destruimos las valianles condicion
  for (int i = 0; i < NUM_PRIORITIES; i++) {
    pthread_cond_destroy(&parking_lot.cond[i]);
  }
  // Eliminamos los mecanimso de arranque de hilos
  pthread_mutex_destroy(&mutex);
  pthread_cond_destroy(&condicion);

  printf("[OK][parking_manager] Finishing\n");

  return 0;
}


// Funcion que valida el formato de las matriculas del archivo CSV
int validar_matricula(char*matricula){
  //1111-AAA: 8 caracteres
  if(strlen(matricula)!=8){
    return -1;
  }
  // Comprobamos que los 4 primeros caracteres sean numeros
  for(int i = 0; i<4; i++){
    if (!isdigit(matricula[i])){
      return -1;
    }
  }
  // Comprobamos que exista la separacion con guion
  if(matricula[4]!='-'){
    return -1;
  }
  // Comprobamos que los 3 ultimos caracteres son letras mayusculas
  for(int i = 5; i<=7; i++){
    if(!isupper(matricula[i])){
      return -1;
    }
  }
  return 0;
}


int leer_linea(int fd, char *linea, ssize_t max_len) {
    // Es importante pasar 'linea' como puntero para que al editarlo en esta funcion,
    // se quede editado para utilizarlo en lectura CSV
    int i = 0; // Contador
    char c;
    ssize_t bytes_leidos;

    while (i < max_len -1){ // Vamos a extraer caracter a caracter
        if((bytes_leidos = read(fd,&c,1))<0){ // Leemos y vemos si hay errores
            perror("ERROR: fallo al leer una linea del archivo CSV");
            return -1;
        }
        if(bytes_leidos == 0){ // Si no podemos leer mas es que hemos llegado al final de la linea
            break;
        }
        linea[i] = c; // Guardamos la letra leida
        i += 1; // Incrementamos el tamano


        if (linea[i-1]=='\n'){// Estamos al final de la linea
            break;
        }
    }
    linea[i] = '\0'; // Anadimos \0 para realizar correctamente la lectura de sscanf
    return i; // Devolvemos la longitud de la linea para saber si hemos llegado al final del archivo CSV
}


int lectura_CSV(char *ruta_fichero) {
  // Abrimos el archivo
  int fd = open(ruta_fichero, O_RDONLY);
  if (fd == -1) {
    perror("ERROR: fallo al abrir el archivo CSV");
    return -1;
  }

  // Establecemos las variables necesarias para la funcion y que utilizaremos mas adelante
  char linea[256]; // Es el buffer que guarda la linea. Lo hemos hecho excesivamente grande
  // para evitar fallos en caso de que haya lineas de matriculas incorrectas en el archivo.
  // En caso de haber lineas incorrectas, serian ignoradas por otras parte del codigo
  char matricula[9]; // 8 caracteres y 1 para el '\0'
  int puerta;
  int prioridad;
  int capacidad = CAPACIDAD_OG_CSV; // Capacidad del parking
  int coches_guardados = 0; // Contador de coches guardados
  int cabecera_raikou = 1; // Para ignorar la cabecera

  vector = (struct car*) malloc((size_t)capacidad * sizeof(struct car)); //hay que reservar memoria inicialmente con malloc para el vector
  if (vector == NULL) {
    perror("ERROR: fallo al reservar memoria inicial para el vector de coches");
    if(close(fd)<0){
      printf("ERROR: fallo al cerrar el fichero CSV\n");
      return -1;
    }
    return -1;
  }

  //vamos a usar esta variable para comprobar si hubo un error de lectura
  int ret_linea;
  // Utilizamos un bucle para leer cada linea usando la funcion auxiliar leer_linea
  while ((ret_linea = leer_linea(fd, linea, sizeof(linea))) > 0) {
    // Al hacer linea en vez de &linea nos aseguramos que editamos la variable para luego escanearla
    // Ignoramos la cabecera
    // Importante resaltar el nombre de raikou, pokemon legendario de tipo electrico
    if (cabecera_raikou) {
    cabecera_raikou = 0;
    continue; // Pasamos a la siguiente linea
    }
    // Usamos sscanf para extraer los datos de la linea
    if (sscanf(linea, "%8[^,],%d,%d", matricula, &puerta, &prioridad) == 3) {
      // Tenemos que tener en cuenta todas las comprobaciones de intervalso del enunciado
      // Primero se comprueba si la matricula es valida en formato usando la funcion validar_matricula
      // Si algun campo fuera errorneo pues pasariamos a la siguiente linea
      if (validar_matricula(matricula) == 0) {
        // Hay 4 puertas [0,3]
        if (puerta >= 0 && puerta <= 3) {
          // Hay 3 prioridades [0,2]
          if (prioridad >= 0 && prioridad <= 2) {
            // Si hemos llegado a este punto es que todos los campos son validos
            // Por lo que tenemos que guardar los datos en el vector de estructuras
            if (coches_guardados == capacidad) { // Comprobamos si hemos ocupado toda la memoria reservada
              capacidad = capacidad * 2; // Duplicamos la memoria reservada

              // Hacemos un realoc para aumentar la memoria reservada
              // Casteamos el valor de la capacidad a size_t para poder hacer el aumento de memoria sin posibles fallos
              struct car *temp = (struct car*) realloc(vector, (size_t)capacidad * sizeof(struct car));
              if (temp == NULL) {
                printf("ERROR: fallo al expandir memoria en del vector de coches\n");
                free(vector); // Liberamos la memoria con un free
                if(close(fd)<0){
                  printf("ERROR: fallo al cerrar el fichero CSV\n");
                  return -1;
                }
                return -1;
              }
              vector = temp; // Actualizamos nuestro puntero
            }
            strcpy(vector[coches_guardados].plate, matricula); // Guardamos la matricula en el vector
            vector[coches_guardados].gate = puerta; // Guardamos la puerta (gate) en el vector
            vector[coches_guardados].priority = prioridad; // Guardamos la prioridad en el vector
            coches_guardados++; // Aumentamos nuestro contador
          }
        }
      }
    }
  }

  //Si leer_linea devolvio -1 es que read() fallo, asi que todo y devolvemos error
  if (ret_linea == -1) {
    fprintf(stderr, "ERROR: fallo durante la lectura del archivo CSV\n");
    free(vector);
    vector = NULL;
    close(fd);
    return -1;
  }

  // Cerrramos el archivo comprovando si hay error
  if (close(fd) == -1) {
    perror("ERROR: fallo al cerrar el archivo CSV");
    return -1;
  }
  return coches_guardados; // Devolvemos el numero de coches que se han procesado correctamente
}


// Comprobamos la priorirdad del vehiculo
// 0:maxima prioridad; 2:minima prioridad
int comprobador_prioridad(Lot *parking, int priority) {
  for (int i = 0; i < priority; i++) {
    if (parking->waiting[i] > 0) return 1;
  }
  return 0;
}

// lot_claim_space(Lot *l): controla el acceso de los hilos a las plazas de parking
void lot_claim_space(Lot *parking, struct car *coche) {
  // Cogemos el mutex
  if (pthread_mutex_lock(&parking->mutex) != 0) {
    fprintf(stderr, "ERROR: fallo en pthread_mutex_lock en lot_claim_space\n");
    return;
  }
  // Entramos a la zona critica del codigo

  // Establecemos el nivel de prioridad del coche
  int prioridad = coche->priority;

  // Incrementamos el contador de esperando
  parking->waiting[prioridad]++;

  // Esperamos mientras el parking este lleno o haya hilos de mayor prioridad esperando
  while (parking->occupied >= parking->capacity || comprobador_prioridad(parking, prioridad)) {
    if (pthread_cond_wait(&parking->cond[prioridad], &parking->mutex) != 0) {
      fprintf(stderr, "ERROR: fallo en pthread_cond_wait en lot_claim_space\n");
      parking->waiting[prioridad]--;
      pthread_mutex_unlock(&parking->mutex);
      return;
    }
  }

  // Decrementamos el contador de coches esperadando
  parking->waiting[prioridad]--;
  // Incrementa el contador de plazas ocupadas
  parking->occupied++;

  // Imprimimos el mensaje en la salida estandar: [INFO] LOT CLAIMED <plazas ocupadas>/<numero de plazas> BY <matricula del vehiculo>
  printf("[INFO] LOT CLAIMED %d/%d BY %s\n", parking->occupied, parking->capacity, coche->plate);

  // Volvemos a comrpobar los hilos que estaban esperando
  for (int i = prioridad + 1; i < NUM_PRIORITIES; i++) {
    if (parking->waiting[i] > 0) {
      if (pthread_cond_broadcast(&parking->cond[i]) != 0) {
        fprintf(stderr, "ERROR: fallo en pthread_cond_broadcast en lot_claim_space\n");
      }
    }
  }
  // Salimos de la zona critica del codigo
  if (pthread_mutex_unlock(&parking->mutex) != 0) {
      fprintf(stderr, "ERROR: fallo en pthread_mutex_unlock en lot_claim_space\n");
    }
}


void lot_release_space(Lot *parking, struct car *coche) {
  //Cogemos el mutex y entramos en la zona critica del codigo para evitar condiciones de carrera
  if (pthread_mutex_lock(&parking->mutex) != 0) {
    fprintf(stderr, "ERROR: fallo en pthread_mutex_lock en lot_release_space\n");
    return;
  }

  // Decrementamos la variable compartida de las plazas ocupadas
  parking->occupied--;

  // Se imprime el mensaje establecido en la practica
  printf("[INFO] LOT RELEASED %d/%d BY %s\n", parking->occupied, parking->capacity, coche->plate);

  // Volvemos a comrpobar los hilos que estaban esperando
  for (int i = 0; i < NUM_PRIORITIES; i++) {
    if (pthread_cond_broadcast(&parking->cond[i]) != 0) {
      fprintf(stderr, "ERROR: fallo en pthread_cond_broadcast en lot_release_space\n");
    }
  }

  // Liberamos el mutex y salimos de la region critica del coodigo
  if (pthread_mutex_unlock(&parking->mutex) != 0) {
    fprintf(stderr, "ERROR: fallo en pthread_mutex_unlock en lot_release_space\n");
  }
}

// Funcion destinada a los hilos de entrada
void * entry_lane_thread(void * arg) {
  // Recogemos los argumentos pasados a la funcion
  ThreadArgs *argumentos = (ThreadArgs *)arg;
  // Si los argumentos son invalidos lanzamos un mensaje de error y finalizamos el hilo
  if (argumentos == NULL){
    fprintf(stderr, "[ERROR][entry_lane_thread] Arguments not valid\n");
    pthread_exit((void *)(long)-1); //devolvemos error
  }

  // Anunciamos que los argumentos han sido procesados y esperamos la senal de arranque
  printf("[LANE %d][GATE %d] Waiting\n", argumentos->priority, argumentos->gate);
  // La funcion se bloquea hasta recibir la senal del parking_manager para permitir el acceso de vehiculos.
  if (pthread_mutex_lock(&mutex) != 0) { //control de erroes
    fprintf(stderr, "[ERROR][LANE %d][GATE %d] fallo en pthread_mutex_lock\n",
            argumentos->priority, argumentos->gate);
    pthread_exit((void *)(long)-1);
  }
  while (bandera == 0){
    //control de errores
    if (pthread_cond_wait(&condicion, &mutex) != 0) {
      fprintf(stderr, "[ERROR][LANE %d][GATE %d] fallo en pthread_cond_wait\n",
              argumentos->priority, argumentos->gate);
      pthread_mutex_unlock(&mutex);
      pthread_exit((void *)(long)-1);
    }
  }
  // Cuando la bandera deja de ser 0 desbloqueamos el mutex y empezamos la ejecucion del carril de entrada
  if (pthread_mutex_unlock(&mutex) != 0) {
    fprintf(stderr, "[ERROR][LANE %d][GATE %d] fallo en pthread_mutex_unlock\n",
            argumentos->priority, argumentos->gate);
    pthread_exit((void *)(long)-1);
  }

  // Realizamos un bucle infinito ya que no sabemos cual es la cantidad de coches a procesar
  // El bucle finaliza con la condicion de salida
  while (1){
    // Recogemos los datos del vehiculo y los guardamos
    struct car *vehiculo;
    // Usamos '->' porque 'argumentos' es un puntero. La flecha hace lo siguiente:
    // se dirige a la dirección de memoria de la estructura y extrae la variable 'entry_lane_queue'.
    vehiculo = queue_get(argumentos->entry_lane_queue);

    // Condicion de salida. Si el vehiculo introducido esta vacio,
    // significa que el productor ya ha procesado todos los vehiculos mediante el CSV
    // Por lo que salimos del bucle
    if (vehiculo == NULL){
      break;
    }
    // Imprimimos los datos del vehiculo al obtenerlo con el mensaje establecido en el enunciado de la practica
    printf ("[LANE %d][GATE %d] Car %s\n", argumentos->priority, argumentos->gate, vehiculo->plate);

    // Mediante la funcion lot_claim_space adquiririmos una plaza en el parking considerando su prioridad.
    // En caso de que el parking este lleno, se bloquea la ejecucion del hilo.
    // Ademas, actualizamos las estadisticas de ocupaciones del parking
    lot_claim_space (&parking_lot, vehiculo);

    // Preparamos al vehiculo para utilizarlo tambien en exit_gate_thread
    // Añadimos el vehiculo a una de las puertas de salida y enviamos sus datos a exit_queue_global
    int numero_salida;
    numero_salida = (argumentos->gate) % NUM_EXIT_GATES;
    //control de error
    if (queue_put(exit_queue_global[numero_salida], vehiculo) != 0) {
      fprintf(stderr, "[ERROR][LANE %d][GATE %d] fallo al insertar vehiculo %s en cola de salida\n",
              argumentos->priority, argumentos->gate, vehiculo->plate);
    }
    // Como queue_put hace una copia de los datos dentro de la cola,
    // debemos liberar la memoria que ocupa queue_get para evitar fugas
    free(vehiculo);
  }

  // Si el productor ha finalizado de agregar elementos a la cola y la cola esta vacia,
  // finalizamos la ejecucion del hilo y retornamos
  printf("[LANE %d][GATE %d] Ending\n", argumentos->priority, argumentos->gate);
  pthread_exit(NULL);
}

// Funcion destinada a los hilos de salida
void * exit_gate_thread(void * arg) {
  // Si los argumentos son invalidos lanzamos un mensaje de error y finalizamos el hilo
  if (arg == NULL){
    fprintf(stderr, "[ERROR][EXIT %d] Arguments not valid\n", -1);
    pthread_exit((void *)(long)-1);
  }
  // Recogemos los argumentos pasados a la funcion
  ThreadArgs *argumentos = (ThreadArgs *)arg;

  // La funcion se bloquea hasta recibir la senal del parking_manager para permitir el acceso de vehiculos.
  if (pthread_mutex_lock(&mutex) != 0) {
    fprintf(stderr, "[ERROR][EXIT %d] fallo en pthread_mutex_lock\n", argumentos->gate);
    pthread_exit((void *)(long)-1);
  }
  while (bandera == 0){
    if (pthread_cond_wait(&condicion, &mutex) != 0) {
      fprintf(stderr, "[ERROR][EXIT %d] fallo en pthread_cond_wait\n", argumentos->gate);
      pthread_mutex_unlock(&mutex);
      pthread_exit((void *)(long)-1);
    }
  }
  // Cuando la bandera deja de ser 0 desbloqueamos el mutex y empezamos la ejecucion del carril de entrada
  if (pthread_mutex_unlock(&mutex) != 0) {
    fprintf(stderr, "[ERROR][EXIT %d] fallo en pthread_mutex_unlock\n", argumentos->gate);
    pthread_exit((void *)(long)-1);
  }

  // Realizamos un bucle infinito para procesar los coches de salida
  while (1){
    // Recogemos los datos del vehiculo y los guardamos
    struct car *vehiculo;
    vehiculo = queue_get(argumentos->entry_lane_queue);

    // Condicion de salida. Si el vehiculo introducido esta vacio,
    // significa que el productor ya ha procesado todos los vehiculos mediante el CSV,
    // por lo que salimos del bucle
    if (vehiculo == NULL){
      break;
    }

    // Imprimimos los datos del vehiculo al obtenerlo
    printf ("[EXIT %d] CAR %s\n", argumentos->gate, vehiculo->plate);

    // Llamamos a la funcion lot_release_space para que al salir el vehiculo del parking, libere su plaza
    lot_release_space(&parking_lot, vehiculo);

    // Tras haber liberado el espacio del parking, realizamos una destruccion segura del coche
    // para evitar fugas de memoria
    free (vehiculo);
  }
  // Si ya no quedan mas elementos en la cola, finalizamos la ejecucion del hilo y retornamos
  printf("[EXIT %d] Ending\n", argumentos->gate);
  pthread_exit(NULL);
}
