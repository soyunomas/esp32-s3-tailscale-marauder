# AGENTS.md

## 1. Piensa antes de modificar
No adivines.
Lee el código relacionado primero.
Si estás confundido: para y resume.

## 2. Cambios mínimos
Toca solo lo necesario.
No refactorices fuera del scope.

## 3. Sigue las convenciones existentes
La consistencia local gana.
No introduzcas nuevos patrones sin motivo explícito.

## 4. Reutiliza antes de crear
Busca exports, utilidades y código similar antes de agregar lógica nueva.

## 5. El código decide; el modelo razona
No uses LLMs para retries, routing, validaciones deterministas o control crítico.

## 6. Verifica de verdad
Tests y checks deben validar intención de negocio, no solo ejecución.

## 7. Falla fuerte
No ocultes errores, registros omitidos ni estados parciales.

## 8. Haz checkpoints
Después de cambios importantes:
- resume estado
- riesgos
- siguiente paso

## 9. Mantén memoria persistente
Actualiza:
- todo.md
- LESSONS.md

## 10. Contexto largo = degradación
Si la sesión se degrada:
- resume
- checkpoint
- reinicia contexto
