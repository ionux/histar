extern "C" {
#include <inc/lib.h>
};

#include <inc/cpplabel.hh>

int
main(int ac, char **av)
{
    printf("Hello world -- from C++.\n");

    label l(1);
    l.set(5, LB_LEVEL_STAR);
    l.set(7, 0);
    printf("label says %s\n", l.to_string());

    return 0;
}
