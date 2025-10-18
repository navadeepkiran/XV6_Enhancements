#include<stdio.h>
#include<stdlib.h>
#include<pthread.h>
#include<unistd.h> // for sleep()
#include<time.h>
#include<string.h>
#define MAX_CUSTOMERS_IN_SHOP 25
#define SOFA_CAPACITY 4
#define NUM_CHEFS 4

// Data Structures for Queues and Customer State
typedef struct QueueNode {
    int customer_id;
    struct QueueNode* next;
} QueueNode;
typedef struct Queue{
    QueueNode * front;
    QueueNode* rear;
}Queue;
typedef struct Customerstate{
    int id;
    int cakeisready;
    int paymentaccepted;
    struct Customerstate* next;
}Customerstate;

// Arguments for customer threads
typedef struct CustomerArgs {
    int id;
    int arrival_timestamp;
} CustomerArgs;

// Mutexes to protect shared resources
pthread_mutex_t g_bakery_mutex;
pthread_mutex_t g_cout_mutex;
pthread_mutex_t g_payment_mutex; // ensures single cash register

// Condition variables for signaling between threads
pthread_cond_t g_cv_sofa_seat_available;
pthread_cond_t g_cv_customer_event;       // For chefs to wait for any customer
pthread_cond_t g_cv_customer_specific_event; // For customers to wait for their specific event

int g_total=0;
int g_sofa=0;

// Queues to manage customer flow
Queue g_standing_customers;
Queue g_waiting_for_cake;
Queue g_waiting_to_pay;

Customerstate* g_customer_states_head = NULL;

struct timespec g_start_time;
int g_all_customers_arrived = 0;

void init_queue(Queue *q){
    q->front=NULL;
    q->rear=NULL;
}
void enqueue(Queue *q,int id){
    QueueNode * temp=(QueueNode*)malloc(sizeof(QueueNode));
    temp->customer_id=id;
    temp->next=NULL;
    if(q->rear==NULL){
        q->front=temp;
        q->rear=temp;
        return;
    }
    q->rear->next=temp;
    q->rear=temp; 
}
int dequeue(Queue * q){
    if(q->front==NULL)return -1;
    QueueNode* temp=q->front;
    int id=temp->customer_id;
    q->front=q->front->next;
    if(q->front==NULL)q->rear=NULL;
    free(temp);
    return id;

}
int peek(Queue *q){
    if(q->front==NULL) return -1;
    return q->front->customer_id;
}
int is_empty(Queue * q){
    if(q->front==NULL)return 1;
    return 0;
}

Customerstate * find_customerstate(int customer_id){
    Customerstate* current = g_customer_states_head;
    while(current!=NULL){
        if(current->id == customer_id){
            return current;
        }
        current=current->next;
    }
    // create
    Customerstate * new=(Customerstate*)malloc(sizeof(Customerstate));
    new->id=customer_id;
    new->cakeisready=0;
    new->paymentaccepted=0;
    new->next=g_customer_states_head;
    g_customer_states_head=new;
    return new;
}
void print(char * name,int id,char * activity,char * details){
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    long timestamp = now.tv_sec - g_start_time.tv_sec;
    pthread_mutex_lock(&g_cout_mutex);
    printf("%ld %s %d %s%s\n",timestamp,name,id,activity,details);
    fflush(stdout);
    pthread_mutex_unlock(&g_cout_mutex);
}

void * chef_function(void * arg){
    int chef_id=*(int *)arg;
    free(arg);
    while(1){
         pthread_mutex_lock(&g_bakery_mutex);
         // Chef waits if there's nothing to do.
        while (is_empty(&g_waiting_to_pay) && is_empty(&g_waiting_for_cake) && !g_all_customers_arrived) {
            pthread_cond_wait(&g_cv_customer_event, &g_bakery_mutex);
        }
        
        if (g_all_customers_arrived && g_total == 0) {
            pthread_mutex_unlock(&g_bakery_mutex);
            break; // Exit the loop to terminate the thread
        }
        int customer_id = -1;
        int is_paying = 0;
               // Priority 1: Accept Payment
        if (!is_empty(&g_waiting_to_pay)) {
            customer_id = dequeue(&g_waiting_to_pay);
            is_paying = 1;
        }
        else if(!is_empty(&g_waiting_for_cake)){
            customer_id = dequeue(&g_waiting_for_cake);
            is_paying=0;
        }
        // if a task is found for chef
        if(customer_id!=-1){
            pthread_mutex_unlock(&g_bakery_mutex); // Unlock while busy
            sleep(1);
            if(is_paying){
                pthread_mutex_lock(&g_payment_mutex); // only one chef at a time
                char details[50];
                snprintf(details, sizeof(details), " Customer %d", customer_id);
                print("Chef", chef_id, "accepts payment for", details);
                sleep(2); // Payment takes 2 seconds
                pthread_cond_broadcast(&g_cv_customer_specific_event);
                pthread_mutex_unlock(&g_bakery_mutex);
                pthread_mutex_unlock(&g_payment_mutex);
                Customerstate * temp=find_customerstate(customer_id);
                temp->paymentaccepted=1;
            }
            else{
                 char details[50];
                snprintf(details, sizeof(details), " Customer %d", customer_id);
                print("Chef", chef_id, "bakes for", details);
                sleep(2); // Baking takes 2 seconds

                pthread_mutex_lock(&g_bakery_mutex);
                Customerstate * temp=find_customerstate(customer_id);
                temp->cakeisready=1;
            }
            // Notify all customers; the correct one will wake up and check its state

            pthread_cond_broadcast(&g_cv_customer_specific_event);
             pthread_mutex_unlock(&g_bakery_mutex);
        }
          else {
             // This else handles the path where no customer was found
             pthread_mutex_unlock(&g_bakery_mutex);
         }
         // There is no extra unlock here. The loop restarts correctly.
    }
    return NULL;
}

void * customer_function(void * arg){
    CustomerArgs* args=(CustomerArgs*)arg; // id,timestamp
    int customer_id=args->id;
    int arrival_timestamp=args->arrival_timestamp;
    free(arg);
        // Wait until the customer's designated arrival time
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    long current_time = now.tv_sec - g_start_time.tv_sec;
    if (arrival_timestamp > current_time) {
        sleep(arrival_timestamp - current_time);
    }

    // enter bakery
    pthread_mutex_lock(&g_bakery_mutex);
    if (g_total >= MAX_CUSTOMERS_IN_SHOP) {
        pthread_mutex_unlock(&g_bakery_mutex);
        return NULL; // Shop is full, customer leaves immediately
    }
    g_total++;
    print("Customer", customer_id, "enters", "");
    pthread_mutex_unlock(&g_bakery_mutex);
    sleep(1);

    //sit on sofa
    pthread_mutex_lock(&g_bakery_mutex);
    if (g_sofa >= SOFA_CAPACITY) {
        enqueue(&g_standing_customers, customer_id);
        while (g_sofa >= SOFA_CAPACITY || peek(&g_standing_customers) != customer_id) {
            pthread_cond_wait(&g_cv_sofa_seat_available, &g_bakery_mutex);
        }
        dequeue(&g_standing_customers);
    }
    g_sofa++;
    print("Customer", customer_id, "sits", "");
    pthread_mutex_unlock(&g_bakery_mutex);
    sleep(1);

    // get cake
    pthread_mutex_lock(&g_bakery_mutex);
    Customerstate* my_state = find_customerstate(customer_id);
    my_state->cakeisready = 0;
    enqueue(&g_waiting_for_cake, customer_id);
    print("Customer", customer_id, "requests cake", "");
    pthread_cond_signal(&g_cv_customer_event); // Wake one chef
        while (!my_state->cakeisready) {
        pthread_cond_wait(&g_cv_customer_specific_event, &g_bakery_mutex);
    }
    pthread_mutex_unlock(&g_bakery_mutex);
    // No sleep here per problem description

    // pay
    pthread_mutex_lock(&g_bakery_mutex);
    my_state->paymentaccepted = 0;
    enqueue(&g_waiting_to_pay, customer_id);
    print("Customer", customer_id, "pays", "");
    //sleep(1);
    pthread_cond_signal(&g_cv_customer_event); // Wake one chef
    pthread_mutex_unlock(&g_bakery_mutex);
    sleep(1);
    pthread_mutex_lock(&g_bakery_mutex);
    while (!my_state->paymentaccepted) {
        pthread_cond_wait(&g_cv_customer_specific_event, &g_bakery_mutex);
    }
    pthread_mutex_unlock(&g_bakery_mutex);
    // The wait for payment acceptance happens inside the wait loop above.

    // get lost

    pthread_mutex_lock(&g_bakery_mutex);
    g_total--;
    g_sofa--;
    print("Customer", customer_id, "leaves", "");
    pthread_mutex_unlock(&g_bakery_mutex);
    
    // Delay to ensure "leaves" and "sits" don't happen in the same second
    sleep(1);
    
    pthread_mutex_lock(&g_bakery_mutex);
    pthread_cond_signal(&g_cv_sofa_seat_available); // Signal one standing customer
    pthread_mutex_unlock(&g_bakery_mutex);

    return NULL;

}


int main(){

    // Initialization
    pthread_mutex_init(&g_bakery_mutex, NULL);
    pthread_mutex_init(&g_cout_mutex, NULL);
    pthread_cond_init(&g_cv_sofa_seat_available, NULL);
    pthread_cond_init(&g_cv_customer_event, NULL);
    pthread_cond_init(&g_cv_customer_specific_event, NULL);
    pthread_mutex_init(&g_payment_mutex,NULL);
    init_queue(&g_standing_customers);
    init_queue(&g_waiting_for_cake);
    init_queue(&g_waiting_to_pay);

    pthread_t chef_threads[NUM_CHEFS];
    pthread_t customer_threads[MAX_CUSTOMERS_IN_SHOP]; // Assume max 25 customers for array size
    int customer_count = 0;

     // Start the chef threads
    for (int i = 0; i < NUM_CHEFS; ++i) {
        int* chef_id = malloc(sizeof(int));
        *chef_id = i + 1;
        pthread_create(&chef_threads[i], NULL, chef_function, chef_id);
    }   
     char line[100];
    int first_timestamp = -1;

    // Read customer arrivals from standard input
    while (fgets(line, sizeof(line), stdin) != NULL) {
        if (strncmp(line, "<EOF>", 5) == 0) {
            break;
        }
        int timestamp, id;
        sscanf(line, "%d Customer %d", &timestamp, &id);
        
        if (first_timestamp == -1) {
            first_timestamp = timestamp;
            clock_gettime(CLOCK_MONOTONIC, &g_start_time);
            // Adjust start time to match first customer's arrival
            g_start_time.tv_sec -= first_timestamp;
        }
        CustomerArgs* args = (CustomerArgs*)malloc(sizeof(CustomerArgs));
        args->id = id;
        args->arrival_timestamp = timestamp;
        pthread_create(&customer_threads[customer_count++], NULL, customer_function, args);
    }
    // Wait for all customer threads to finish
    for (int i = 0; i < customer_count; ++i) {
        pthread_join(customer_threads[i], NULL);
    }
    // Signal to chefs that no more new customers will arrive
    pthread_mutex_lock(&g_bakery_mutex);
    g_all_customers_arrived = 1;
    pthread_cond_broadcast(&g_cv_customer_event); // Wake all chefs to check shutdown condition
    pthread_mutex_unlock(&g_bakery_mutex);

        // Wait for all chef threads to finish
    for (int i = 0; i < NUM_CHEFS; ++i) {
        pthread_join(chef_threads[i], NULL);
    }
        // Cleanup
    pthread_mutex_destroy(&g_bakery_mutex);
    pthread_mutex_destroy(&g_payment_mutex);
    pthread_mutex_destroy(&g_cout_mutex);
    pthread_cond_destroy(&g_cv_sofa_seat_available);
    pthread_cond_destroy(&g_cv_customer_event);
    pthread_cond_destroy(&g_cv_customer_specific_event);

    // Free the linked list of customer states
    Customerstate* current = g_customer_states_head;
    while(current != NULL) {
        Customerstate* temp = current;
        current = current->next;
        free(temp);
    }
    return 0;
}