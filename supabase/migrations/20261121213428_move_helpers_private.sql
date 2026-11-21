-- Keep the RLS helper functions out of the public API (security advisor fix)
create schema if not exists private;
alter function public.is_admin() set schema private;
alter function public.can_use_device(uuid) set schema private;
alter function public.handle_new_user() set schema private;
revoke all on schema private from public, anon;
grant usage on schema private to authenticated;
revoke all on function private.is_admin() from public, anon;
revoke all on function private.can_use_device(uuid) from public, anon;
grant execute on function private.is_admin() to authenticated;
grant execute on function private.can_use_device(uuid) to authenticated;
-- can_use_device calls is_admin by name; point it at the new schema
create or replace function private.can_use_device(d uuid) returns boolean
language sql stable security definer set search_path = public as $$
  select private.is_admin()
      or exists (select 1 from public.devices where id = d and claimed_by = auth.uid())
      or exists (select 1 from public.device_access where device_id = d and user_id = auth.uid());
$$;
delete from public.devices where chip_id = 'TEST-CHIP-001';
