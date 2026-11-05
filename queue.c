
#include "queue.h"
#include <stdio.h>

//Primero creamos la cola. Retornamos la direccion de memoria donde estara la primera posicion de la cola.
//Posteriormente se asiganara en el main al vector que contiene las colas circulares de cada hilo
queue* queue_init(int size){
    // Declaramos una variable puntero a la estrcutura queue
    queue *q;
    // Liberamos el espacio necesario en memoria de una estructura queue
    // Hay que hacer un cast de malloc para indicar que el puntero q apunta a una estrcutra queue (no a void)
        q = (queue *)malloc(sizeof(queue));
    // Control de errores malloc
    if(q == NULL){
        fprintf(stderr,"ERROR: no fue posible realizar el malloc\n");
        return NULL; // No retornamos -1 porque la funcion devuelve un puntero, NO un int
    }
    // Inicializamos el puntero por el que saco coches a la posicion 0
    q->head = 0;
    // Inicializamos el puntero por el que meto coches a la posicion 0
    q->tail = 0;
    // Inicializamos el numero inicial de coches a 0
    q->count = 0;
    // El tamano de la cola sera size, y viene dado como argumento de la funcion
    q->size = size;
    //incializamos el campo que suaremos para comprobar si ha terminado el hilo
    q->terminado = 0;
    // data es un puntero a la posicion del primer coche (struct car). Como no sabemos el numero de coches que tendremos por cola, NO podemos hacer un array fijo [n]
    // y tendremos que utilizar memoria dinámica para reservar el espacio adecuado y necesario.
    // Asi, data apuntara a la primera posicion de memoria de un espacio de memoria donde se almacenaran los coches
    q->data = (struct car*)malloc((size_t)size * sizeof(struct car)); // Hay que hacer cast a size_t, malloc espera size_t NO int
    if(q->data == NULL){
        fprintf(stderr,"ERROR: no fue posible realizar el malloc\n");
        free(q); // En caso de que fallase este malloc, liberamos lo que ya habiamos reservado en q para evitar fugas
        return NULL; // No retornamos -1 porque la funcion devuelve un puntero, NO un int
    }
    // Inicializamos mutex y variables condicion y realizamos el cotnrol de errores correspondiente
        if(pthread_mutex_init(&q->mutex, NULL) != 0){
        fprintf(stderr,"ERROR: fallo al inicializar el mutex de la cola\n");
        free(q->data);
        free(q);
        return NULL;
    }

    if(pthread_cond_init(&q->cond_not_full, NULL) != 0){
        fprintf(stderr,"ERROR: fallo al inicializar cond_not_full de la cola\n");
        pthread_mutex_destroy(&q->mutex);
        free(q->data);
        free(q);
        return NULL;
    }

    if(pthread_cond_init(&q->cond_not_empty, NULL) != 0){
        fprintf(stderr,"ERROR: fallo al inicializar cond_not_empty de la cola\n");
        pthread_cond_destroy(&q->cond_not_full);
        pthread_mutex_destroy(&q->mutex);
        free(q->data);
        free(q);
        return NULL;
    }

    return q;
}


// Funcion para introducir un coche en la cola
int queue_put(queue *q, struct car* x) {
    // Utilizamos mutex y variable condicion para proteger la seccion critica
    //tenemos que realizar el control de errores necesario
    if(pthread_mutex_lock(&q->mutex) != 0){
        fprintf(stderr,"ERROR: fallo en pthread_mutex_lock en queue_put\n");
        return -1;
    }
    // Si la cola esta llena, me espero para poder introducir otro coche
    while(queue_full(q)){
        //tenemos que realizar el control de errores necesario
        if(pthread_cond_wait(&q->cond_not_full, &q->mutex) != 0){
            fprintf(stderr,"ERROR: fallo en pthread_cond_wait en queue_put\n");
            pthread_mutex_unlock(&q->mutex);
            return -1;
        }
    }
    // Meto un coche en el array dinamico data (si ya habia un coche en esa posicion, se sobreescribe)
    // El argumento x viene dado como un puntero, por lo que como queremos solo su contenido, añadimos *
    q->data[q->tail] = *x;
    // Desplazo el tail
    q->tail = (q->tail +1) % q->size;
    // Sumo uno a los coches que hay en la cola
    q->count += 1;
    // Mando una señal para un hilo (aleatorio) que esta esperando a que la cola NO este vacia
    //tenemos que realizar el control de errores necesario
    if(pthread_cond_signal(&q->cond_not_empty) != 0){
        fprintf(stderr,"ERROR: fallo en pthread_cond_signal en queue_put\n");
        pthread_mutex_unlock(&q->mutex);
        return -1;
    }
    if(pthread_mutex_unlock(&q->mutex) != 0){
        fprintf(stderr,"ERROR: fallo en pthread_mutex_unlock en queue_put\n");
        return -1;
    }
    return 0;
}

// Funcion para sacar un coche en la cola. Retorna un puntero que apunta a una struct car
struct car* queue_get(queue *q) {
    struct car* element;
    // Utilizamos mutex y variable condicion para proteger la seccion critica
    //hay que realizar el control de errores
    if(pthread_mutex_lock(&q->mutex) != 0){
        fprintf(stderr,"ERROR: fallo en pthread_mutex_lock en queue_get\n");
        return NULL;
    }
    // salimos del wait si la cola esta vacia o ha terminado
    while(queue_empty(q) && !q->terminado){
        //hay que realizar el control de errores
        if(pthread_cond_wait(&q->cond_not_empty, &q->mutex) != 0){
            fprintf(stderr,"ERROR: fallo en pthread_cond_wait en queue_get\n");
            pthread_mutex_unlock(&q->mutex);
            return NULL;
        }
    }
    //si la cola esta vacia y la hemos marcado como terminado no hay mas coches
    if (queue_empty(q)) {
        pthread_mutex_unlock(&q->mutex);
        return NULL;
    }
    // Para 'proteger' el coche que saco de la cola antes cualquier cambio, lo mejor es que reserve espacio en memoria para hacer una copia del coche
    element = (struct car*)malloc(sizeof(struct car));
    if(element == NULL){
        fprintf(stderr,"ERROR: no fue posible realizar el malloc\n");
        pthread_mutex_unlock(&q->mutex); // En caso de error es importante liberar el mutex para que NO se quede cogido para siempre
        return NULL; // No retornamos -1 porque la funcion devuelve un puntero, NO un int
    }
    *element = q->data[q->head];
    // Desplazo el head
    q->head = (q->head +1) % q->size;
    // Resto uno a los coches que hay en la cola
    q->count -= 1;
    // Mando una señal para un hilo (aleatorio) que esta esperando a que la cola NO este llena
    //tambien realizamos el control de errores
    if(pthread_cond_signal(&q->cond_not_full) != 0){
        fprintf(stderr,"ERROR: fallo en pthread_cond_signal en queue_get\n");
        // El elemento ya fue extraido, lo liberamos
        free(element);
        pthread_mutex_unlock(&q->mutex);
        return NULL;
    }

    if(pthread_mutex_unlock(&q->mutex) != 0){
        fprintf(stderr,"ERROR: fallo en pthread_mutex_unlock en queue_get\n");
        free(element);
        return NULL;
    }

    return element;
}


// Comprobamos si la cola esta lleno o vacia. Si esta llena o vacia devuelve 1, si no, devuelve 0
int queue_empty(queue *q){
    if(q->count == 0){
        return 1;
    }else{
        return 0;
    }
}
int queue_full(queue *q){
        if(q->count == q->size){
                return 1;
        }else{
                return 0;
        }
}


// Funcion para liberar y destruir los recursos utilizados
int queue_destroy(queue *q){
    // Es muy importante tener en cuenta el orden de 'destruccion' en esta funcion.
    // NO podemos empezar por q, porque nos quedariamos sin acceso para el resto de elementos a destruir
    // Primero mutex y variables condicion
    int ret = 0; //variable para acumular errores

    // Primero mutex y variables condicion
    if(pthread_mutex_destroy(&q->mutex) != 0){
        fprintf(stderr,"ERROR: fallo al destruir el mutex de la cola\n");
        ret = -1;
    }
    if(pthread_cond_destroy(&q->cond_not_full) != 0){
        fprintf(stderr,"ERROR: fallo al destruir cond_not_full de la cola\n");
        ret = -1;
    }
    if(pthread_cond_destroy(&q->cond_not_empty) != 0){
        fprintf(stderr,"ERROR: fallo al destruir cond_not_empty de la cola\n");
        ret = -1;
    }

    // Luego data
    free(q->data);

    // Por ultimo queue
    free(q);

    return ret;
}
