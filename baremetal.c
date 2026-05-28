// baremetal.c
volatile int x = 0;

int main() {
    for (int i = 0; i < 10; i++) {
        x += i;
    }

    return 0;  // 返回也没关系，会进入死循环
}
